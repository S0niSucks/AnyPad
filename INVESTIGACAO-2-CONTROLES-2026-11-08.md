# Investigação: controle 2 sem resposta com 2× DualSense (USB) + 2× 8BitDo Ultimate v1 (Bluetooth)

Data: 2026-11-08 · AnyPad 0.9.5-beta (checkout local) · análise estática + testes host (container gcc:14).

## Sintoma relatado

Dois DualSense ligados por cabo (gerenciados pelo sistema) e dois 8BitDo Ultimate v1
conectados por Bluetooth pelo AnyPad. Ao conectar e usar, o controle 2 parou de
responder / não funcionou direito.

## Como o AnyPad detecta e conecta controles (resumo do fluxo)

1. **Busca**: `host.c` faz inquiry periódico na janela de pareamento (`host_pair` →
   `start_inquiry`). Controles com classe de dispositivo de gamepad (`on_found`,
   `profile_class_is_gamepad`) viram candidatos.
2. **Exclusões**: DualSense identificados por VID/PID (`054c:0ce6/0df2`) são banidos
   da busca (lista em memória + `ban.txt`). Os DualSense **por USB não aparecem na
   busca** — eles não consomem slots nem bloqueiam nada.
3. **Chamada**: `pair_with()` cria um link em `links[HOST_MAX_PADS=4]` e envia
   `HCI Create Connection`. Um controle por vez (um `Create Connection` por vez no chip).
4. **Recuperações**: colisão (o controle chama o console ao mesmo tempo, status 0x0B),
   "no resources" do SDP (recusa 4 — comportamento documentado do próprio 8BitDo),
   repage após page timeout, EIR vazio (defer).
5. **Pronto**: canais L2CAP SDP/HID abertos → parser do perfil → `ready` → primeiro
   input → `on_connect(slot)` → `vpad_add(slot)` → `scePadVirtualDeviceAddDevice` →
   identificação pelo log do kernel → bind ao usuário → DualSense virtual.

## Causa-raiz encontrada (confirmada por análise de `host.c`/`le.c`/`hci_usb.c`)

**O estado de recuperação de pareamento era por-host (compartilhado por todos os
controles), desenhado para exatamente um controle por janela de busca.** Com dois
8BitDo conectando na mesma janela:

| Estado compartilhado | Efeito com 2 controles |
|---|---|
| `h->turned_down` (orçamento de 3 para colisão 0x0B) | Os dois controles **gastam o mesmo orçamento**. Esgotado, o segundo controle caía em `collision_blocked` e **nunca mais era chamado naquela busca** → morto. |
| `h->fresh_left = 1` + `h->last_call` (endereço único) na recuperação de "no resources" do SDP | Só o **último** controle chamado, **uma vez por busca**, ganhava link novo. O segundo 8BitDo esperava 8 recusas com backoff (~30 s) até cair no timeout `T_SETUP_TIMEOUT` → derrubado. |
| Loop do Command Status (status 0x0B) dava `link_free` em **todos** os links pendentes | A recusa do controle B **matava o link pendente do controle A** silenciosamente. |
| `h->defer_rec` único | Segundo controle respondendo inquiry com EIR vazio era descartado sem estado. |
| `repage_left`/`repage_at` keyed em `last_call` | Só o último controle chamado ganhava retentativas de page timeout. |

Nota: no drop por "no resources" o `last_call` **não** era atualizado para o controle
derrubado — se outro controle foi chamado depois, o repage chamava o controle errado.

## Correções aplicadas (src/host_int.h, src/host.c)

1. **Orçamentos por controle, por endereço**: `turned_down` e `fresh_left` saem de
   `struct host` (um contador global) para uma tabela `pad_budget[HOST_MAX_PADS]`
   **keyed por endereço**, zerada a cada busca (`host_pair`). Cada controle tem
   `TURNDOWN_TRIES=3` e `FRESH_LINK_TRIES=1` seus, e o orçamento **persiste quando o
   link dele é derrubado e recriado** (por link puro não serve: renovaria a cada fresh
   link — defeito pego pelo `test_hearbridge`).
2. **Fresh link**: condição `h->fresh_left > 0 && last_call == l->addr` trocada por
   `l->fresh_left > 0`, e `h->last_call` agora é setado para o controle derrubado
   antes do repage (chama o controle certo, não o último chamado).
3. **Command Status 0x0B**: o handler agora ignora links que não sejam do controle
   efetivamente chamado (`last_call`), então a recusa de um não mata o link pendente
   do outro; usa `l->turned_down` (orçamento próprio).
4. `host_pair` não reseta mais os contadores (cada link novo já nasce com o orçamento).
5. **Candidatos com EIR vazio (defer) por controle**: `defer_rec`/`defer_at` únicos em
   `struct host` viraram `defer_rec[4]/defer_at[4]` — cada controle achado sem resposta
   completa espera a sua (`on_found`), o consumo no `host_poll` chama **um por poll**
   (o mais antigo pronto), todos expiram juntos no fim da janela, e o DualSense cancela
   só a sua entrada. Antes, o segundo controle com EIR vazio era descartado sem estado.

Objetivo: o AnyPad tratar **qualquer cenário** — 4 controles ligados juntos na mesma
janela de busca, respostas incompletas, colisões de chamada e reconexões, cada um com
seu próprio orçamento de recuperação. Comportamento com **um** controle permanece
idêntico (mesmo orçamento, mesmos caminhos) — requisito do upstream ("must not alter
how the DualShock 4 and the 8BitDo connect").

## Validação no console (2026-11-09, build 0720217)

Logs reais (`anypad.log` de duas sessões) confirmaram:

1. **Bug original corrigido no hardware**: os dois 8BitDo (`E4:17:D8:67:C5:6F`,
   `E4:17:D8:E0:A7:92`) conectaram **juntos**, `virtual DualSense created` nos
   slots 0 e 1, em várias sessões.
2. **Novo problema encontrado e corrigido** (build `ca73c8a`): DualSense do
   **sistema** tentando conectar sem fio falhava em loop — o yield de 30 s
   expirava no meio do setup do sistema (auth + canais HID) após o retry de
   ~22 s; o AnyPad voltava a ler eventos e roubava o resto do pareamento
   (`44:46:48:C8:EE:C7` às 6406 s e 6538 s; `90:B6:85:13:B3:1B` às 4039 s).
   Correção: `T_YIELD_SYS = 60000` para chamadas de controles do console.
   Teste `test_pair_waits_for_pause` ajustado; 11/11 passando.

## Resultado dos testes

Suíte completa no container `gcc:14` com `-D_DEFAULT_SOURCE` (Windows host via Docker),
após todas as correções — **11/11 testes passando**:

```
===== RUN test_usb =====        ok
===== RUN test_config =====     ok
===== RUN test_vpad =====       ok   (linhas "scePadInit failed" são cenário de teste)
===== RUN test_crypto =====     ok
===== RUN test_profiles =====   ok
===== RUN test_host =====       ok   (inclui test_pad_own_call_turned_down,
                                      test_fresh_link_on_no_resources,
                                      test_repage_after_timeout — os caminhos alterados)
===== RUN test_le =====         ok   (linhas "pairing failed: timed out" são cenários)
===== RUN test_compat =====     ok
===== RUN test_web =====        ok   (após restaurar web_page.c, ver abaixo)
===== RUN test_launcher =====   ok
===== RUN test_hearbridge ===== ok   (3+8 channel requests: fresh link + 8 recusas)
ALL TESTS PASSED
```

Notas:
- `make test` puro falha no Linux por dois motivos de plataforma, não de código:
  falta de `-D_DEFAULT_SOURCE` (clock_gettime) e `test_lock` exigir `sys/sysctl.h`
  (FreeBSD) — limitação já documentada em ULTIMATE-2.4G-PS5-13.60.md.
- **Bug intermediário pego pela suíte**: a primeira versão da correção (orçamento
  por link) fazia o orçamento zerar a cada fresh link (loop infinito de fresh links);
  o `test_hearbridge` (cenário 4: recusas para sempre) detectou. Corrigido com
  orçamento **por endereço, por janela de busca** (`pad_budget` em `struct host`).
- **`src/web_page.c`**: o working tree local tinha um stub de 153 bytes (array vazio —
  provável geração acidental sem `xxd` no Windows), que quebrava o `test_web`
  (título da página) e a própria página do AnyPad no console. Restaurado do commit
  (`git checkout -- src/web_page.c`); o Makefile o regenera de `web/index.html`
  quando necessário.
- Como reproduzir: `tools/run_linux_tests.sh` equivalente —
  `docker run --rm -v "$PWD:/work" -w /work gcc:14 sh /scripts/run_anypad_tests.sh`
  (script em `C:\Users\Leonardo Mello\AppData\Local\Temp\opencode\run_anypad_tests.sh`).

## Compatibilidade com jogos (superfície verificada no código)

**O que funciona e chega ao jogo** (via `ScePadData` no `scePadVirtualDeviceInsertData`):

| Recurso | Status | Onde |
|---|---|---|
| 16 botões padrão (✕○△□, d-pad, L1/R1/L3/R3, Create/Options, PS via combo, touchpad-click) | ✅ todos os perfis | `pad.h`, perfis em `profiles.c` |
| Analógicos (4 eixos) + gatilhos analógicos L2/R2 (0–255) | ✅ | `ps5_vpad.c send_frame` |
| Touchpad (2 pontos, com ID) | ✅ DS4/DualSense-family parsers | `st->has_touch` |
| Motion (giroscópio + acelerômetro, unidades Sony) | ✅ DS4/Switch Pro-family | `st->has_motion` |
| Bateria/LEDs de jogador no controle | ✅ por perfil (`ds5_output`, `switch_init`, `xbox_output`, `wii_init`) | `k_player_colours` por slot |

**O que NÃO chegava ao jogo (parcialmente resolvido em `75db5fe`, pendente de validação no console):**

| Recurso | Status | Evidência |
|---|---|---|
| **Vibração/rumble vinda do jogo** | 🧪 **ligada** no `75db5fe`: `vpad_poll` lê `scePadVirtualDeviceGetRemoteSetting` por slot (motores `[3..4]`, lightbar `[5..7]`, layout comprovado em PS4 pelo OneFamily) e encaminha ao `host_set_output` → perfil. Guardas: rate-limit 20 ms, keepalive 100 ms, stop 3×50 ms, stale 4 s. Teste novo em `test_vpad.c`; falta confirmar no console (layout do setting pode diferir no firmware 13.60). |
| Lightbar comandada pelo jogo | 🧪 idem (bytes `[5..7]`, escala 0..63 → ×4); cor inicial por slot mantida fora de wishes | — |
| Áudio/fone DualSense | ❌ fora de escopo (HID apenas) | — |

**Implicação prática**: em jogos, os controles Bluetooth funcionam como entrada
completa (menu + jogo + emuladores), mas **efeitos de vibração do jogo não
vibram** e a lightbar não muda de cor. LEDs de jogador e rumble *inicial* do
controle funcionam por perfil (o DS4 tem `rumble_refresh_ms`). Para rumble real,
seria necessário ligar a leitura de output do dispositivo virtual ao
`host_set_output` — mudança separada, não feita aqui.

**Cuidados de compatibilidade documentados** (COMPATIBILITY.md / ULTIMATE-2.4G-PS5-13.60.md):

- Verificado em PS5 fat **10.01** (3 controles: DS4 v2, 8BitDo SF30 Pro Switch-mode,
  GameSir G7 SE). Firmware **13.60 não qualificado** (chamadas `scePadVirtualDevice*`/MBus
  não validadas nessa versão) — o caso do usuário é exatamente essa combinação.
- 8BitDo **Ultimate 2.4G** (`2dc8:3012`): suportado no código via perfil **genérico**
  (mapeamento Android), não testado no PS5. O Ultimate v1 em modo Bluetooth depende do
  VID/PID anunciado: se anunciar `057e:2009` (Switch Pro) usa o parser Switch; se
  `2dc8:3012`, o genérico.
- Switch Pro family: rumble "not done yet for these" (COMPATIBILITY.md).
- Issues upstream relacionadas: #7 (regressão de pareamento/controle travado), #2
  (page timeout Switch Pro) — consistentes com a causa-raiz encontrada.

## Riscos residuais / próximos passos recomendados

1. **Identificação do DualSense virtual** (`ps5_vpad.c`): o servidor de log (TCP 3232)
   pode **replayar** linhas `DEVICE_ADDED` antigas para uma nova conexão; o dreno
   inicial (`log_source_read` pós-open) é non-blocking e pode perder esse replay →
   `g_matches == 2` → "virtual pad not identified (2 candidates)" → controle conectado
   **sem** DualSense virtual. Recomendação: marcar timestamp/`dev` já vistos por slot
   e ignorar `DeviceId` já atribuído a outro slot READY. **Cobre 4 controles**: a
   identificação é serializada (uma por vez), então o risco cresce com cada slot novo.
2. **Transporte morto mascarado**: `h->last_rx` é atualizado por **qualquer** link;
   com controles conectados e **todos** idle >20 s, 3 probes (2 s) declaram o transporte
   perdido e **derrubam todos**. Janela pequena, mas real.
3. **Ban list cheia (16)** → `discovery_blocked` bloqueia pareamento Classic pela
   sessão inteira.
4. **Simulador de 1 pad só** (`tests/sim.c`): os testes existentes não conseguem
   exercitar 2+ controles no mesmo host (estado único de link em `struct sim`).
   Follow-up recomendado: estender o simulador para N pads e adicionar teste de
   regressão do cenário "4 ligando juntos". Enquanto isso, a validação multi-pad é
   pelos marcadores de log no console (abaixo).
5. **Firmware 13.60**: validar `scePadVirtualDevice*` + MBus bind nessa versão antes
   de culpar o fluxo Bluetooth.

## Como detectar futuros bugs (marcadores de log para grep)

Depois de reproduzir no console (`/data/anypad/anypad.log` via página web), procurar:

| Grep no log | Significado |
|---|---|
| `no free slot` | 4 slots de link ocupados (controles LE + Classic juntos) |
| `exclusion list full` / `discovery blocked` | ban list cheia bloqueou pareamento |
| `collision` / `no call of its own was pending` | colisão de chamada (caminho corrigido) |
| `no resources for a channel` | recuperação fresh-link (agora por controle) |
| `channel refused eight times` | controle sem recursos derrubado após 8 recusas |
| `setup did not finish` | timeout de 30 s do setup (controle 2 morrendo) |
| `connection refused (status` | recusa do Create Connection — ver qual controle |
| `virtual pad not identified` | DualSense virtual não criado (risco 2 acima) |
| `the virtual pad refused input` | handle virtual inválido/expirado |
| `transport lost` / `controller silent` | transporte USB/HCI declarado morto |
| `ready as ... in slot N` + `virtual DualSense` | sucesso por slot — comparar slots dos 2 controles |

Reprodução sugerida do bug original: com os 2 DualSense no cabo, abrir o AnyPad,
pressionar Pair e ligar os **dois** 8BitDo juntos na mesma janela (~10 s). Antes da
correção, o log mostraria `turned down` ×3 seguido de `collision` para o segundo
controle, ou `channel refused` até `setup did not finish`. Depois da correção, ambos
devem chegar a `ready as` e `virtual DualSense` em slots distintos.
