# Ultimate 2.4G sem dongle no PS5 13.60 — qualificação

Estado em 26/10/2026: **não qualificado no console**. Esta nota não é uma matriz de compatibilidade nem uma instrução para carregar um ELF sem supervisão.

## Alvo e evidências

- Controle fotografado pelo proprietário: etiqueta **8BitDo Ultimate 2.4G Controller**, chave traseira **D/X**, dock e receptor USB. Não é o Ultimate Bluetooth com chave S/2.4G.
- Console informado: PS5 **13.60**. A versão do AnyPad deste checkout é **0.9.5-beta**, commit `44b8312c9abaf4aaff03d0c74c62a4da010c0108`.
- Relato do proprietário: ligou o controle, segurou **Profile por 5 s** para alternar de 2.4G para Bluetooth, selecionou **D**, segurou **★ por 3 s** e conseguiu pareá-lo com celular/tablet como **“8BitDo Ultimate wireless”**. Também relatou identificação como **“Generic HID”**; ainda falta determinar em qual aplicativo/dispositivo essa indicação apareceu. O pareamento no telefone comprova Bluetooth direto neste controle, mas não comprova que o AnyPad no PS5 tenha recebido relatórios de entrada.
- A [FAQ da 8BitDo para o Ultimate 2.4G](https://support.8bitdo.com/faq/ultimate-2-4g-controller.html) ensina a usar **o receptor 2.4G** no Switch. Isso não demonstra pareamento Switch Pro por Bluetooth direto. O [manual do produto](https://download.8bitdo.com/Manual/Controller/Ultimate/Ultimate-2.4g-wireless-Controller.pdf) descreve Bluetooth para outros hosts, em linha com o pareamento relatado pelo proprietário. Identificar a versão de firmware do controle e o modo efetivo antes de escolher um parser.
- `COMPATIBILITY.md` lista `2dc8:3012` como **suportado no código, não testado no PS5**. Em `src/generic.c`, esse ID seleciona um **mapeamento Android do leitor HID genérico**; não é um perfil Switch e não é evidência de que o Ultimate anuncie esse ID por Bluetooth. `src/profiles.c` reserva o parser Switch Pro para a família `057e:2009` e afins. Sem SDP/HID do controle real, não converter `2dc8:3012` para Switch Pro às cegas.
- README/AI-WORKFLOW do AnyPad relatam teste em PS5 fat **10.01**, não 13.60. O próprio upstream pede ajuda para 13.60. Chamadas `scePadVirtualDevice*`/MBus em `src/ps5_vpad.c` não estão qualificadas nessa versão de firmware.
- Há [relatos de regressão de pareamento/controle travado](https://codeberg.org/elmonomalva0/Any-Pad-ps5/issues/7) e [de pareamento Switch Pro com timeout](https://codeberg.org/elmonomalva0/Any-Pad-ps5/issues/2). São relatos externos em outros ambientes, não resultados deste controle; recomendam cautela.

## Resultado host reproduzível

No checkout limpo, `make test` no container `gcc:14` não termina: primeiro falta declarar `clock_gettime` com as flags originais; com `-D_DEFAULT_SOURCE`, o teste `test_lock` requer `sys/sysctl.h` (FreeBSD, ausente nesse Linux). Isso **não** é uma falha demonstrada no PS5. Os testes direcionados abaixo compilam e passam no container, usando simulação, sem tocar o console:

```sh
docker run --rm -v "$PWD:/work" -w /work gcc:14 make build/test_profiles build/test_compat 'HOST_CFLAGS=-std=c11 -Wall -Wextra -Werror -O1 -g -Isrc -D_DEFAULT_SOURCE'
docker run --rm -v "$PWD:/work" -w /work gcc:14 sh -c './build/test_profiles >/dev/null 2>&1 && ./build/test_compat >/dev/null 2>&1'
```

Esses testes não simulam um Ultimate 2.4G real nem a ABI do PS5 13.60.

## Proteção de logoff no checkout local

`src/ps5_vpad.c` agora confere se o usuário escolhido para os controles virtuais permanece na lista de usuários logados. `src/ps5_main.c` **não abre o Bluetooth se esse usuário não está logado** e encerra a sessão quando ele sai, antes da próxima leitura do chip. O teste `tests/test_vpad.c` cobre a saída do dono quando há outro usuário ainda conectado e a ausência de qualquer usuário. `build/test_vpad` foi compilado e passou no container `gcc:14`.

**Limite:** isso reduz a janela em que AnyPad mantém leituras USB durante a tela de login; **não demonstra cura da falha do DualSense**. O Bluetooth é compartilhado com o driver do sistema; se AnyPad capturar um evento de desconexão que o sistema deveria receber antes de detectar o logoff, o driver pode manter um estado incorreto. Não há evidência de que esse evento possa ser devolvido com segurança. Até testar em hardware com recuperação planejada, **parar AnyPad pela página antes de sair da conta**. Não instalar esta alteração como correção garantida no PS5 13.60.

## Portões antes de mudar o parser

1. **Host, sem PS5:** o pareamento Bluetooth direto no celular/tablet foi relatado; confirmar firmware do controle, serviço HID e, se possível, nome, VID/PID e descritor/relatórios HID anunciados nesse modo. O ID visto pelo receptor USB não serve como substituto. Não coletar nem publicar chaves de pareamento.
2. **Console 13.60, somente com consentimento operacional:** estabelecer que o ELF loader e as permissões necessárias estão funcionais na *mesma sessão* e que o Bluetooth e o DualSense permanecem íntegros. A existência de um ELF em disco, log histórico ou teste no PC não basta.
3. **AnyPad isolado:** iniciar sem outro payload disputando o rádio/porta 8095. Manter o DualSense ligado. Não enviar HCI Reset nem reiniciar o chip compartilhado. Observar primeiro as linhas de firmware/chip e a disponibilidade do pad virtual; se a interface travar, interromper o ensaio e não insistir com variações cegas.
4. **Pareamento do Ultimate:** se encontrado, registrar apenas linhas relevantes de descoberta → autenticação → SDP/GATT/descritor → primeiro relatório → `ready as` → `virtual DualSense` → bind ao usuário → entrada no menu/jogo. Revisar endereço Bluetooth e ID de usuário antes de compartilhar o log; nunca compartilhar `pads.db`.
5. **Só com relatório real:** se `057e:2009` e report `0x30/0x3f`, testar o parser Switch; se `2dc8:3012`, validar o genérico e seus botões; para outro ID/relatório, preparar um teste de simulador que falhe antes da alteração e um perfil específico, sem modificar o fluxo Bluetooth comum.

Critério de avanço: entrada responsiva em menu e jogo PS5, saída limpa e DualSense ainda funcional. Reconexão, gatilhos analógicos, vibração, touchpad, movimento e coexistência com áudio Bluetooth são **etapas separadas**, não implicadas por pareamento ou pela criação do controle virtual.

Fontes locais de contexto, sem validação deste ensaio: `Documentation/docs/knowledge-base/originals/docs/research-2026-09-29/46-relapse-1360-evidence.md` (linhas 20–26; logs históricos de outra sessão) e `Documentation/docs/knowledge-base/originals/docs/research-2026-09-30/topics/43.md` (linhas 26–45; separação entre SDK, loader e execução).
