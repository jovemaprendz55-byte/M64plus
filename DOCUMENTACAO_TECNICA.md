# Documentação técnica — M64plus

> **Objetivo:** registrar a pesquisa de arquitetura do Mupen64Plus AE como referência técnica para o desenvolvimento de um emulador Nintendo 64 para Android, com atenção ao Android 16 e ao Poco C85 (Unisoc T7250 / Mali-G52 MC2).
>
> **Estado:** documento misto de pesquisa e acompanhamento de implementação. As seções sobre Mupen64Plus AE descrevem apenas o projeto de referência; as seções 17 e 18 registram o estado observado no código do M64plus. Não confundir pesquisa, implementação parcial e validação no aparelho.
>
> **Limite importante:** este estudo não autoriza copiar arquivos isolados do Mupen64Plus AE nem importar binários/plugins sem compreender e integrar suas dependências e licenças. Nenhum código do projeto de referência foi incorporado por este documento.

## 1. Repositório e escopo

- Repositório: [M64plus](https://github.com/jovemaprendz55-byte/M64plus)
- Branch documentada: `main`
- Projeto de referência: [mupen64plus-ae/mupen64plus-ae](https://github.com/mupen64plus-ae/mupen64plus-ae)
- Projeto anterior `N64-Android`: manter separado; esta documentação não altera aquele repositório.
- Plataforma-alvo em estudo: Android 16, ABI ARM64, aparelho Poco C85 com SoC Unisoc T7250 e GPU Mali-G52 MC2.

## 2. Arquitetura geral do Mupen64Plus AE

O AE é um frontend Android que integra o núcleo Mupen64Plus e vários plugins/módulos nativos. Não é um único binário monolítico.

Componentes identificados no repositório de referência:

- `mupen64plus-core`: núcleo da emulação, CPU MIPS, dispositivos, memória e API.
- `mupen64plus-rsp-hle`, `mupen64plus-rsp-cxd4`, `mupen64plus-rsp-parallel`: implementações RSP.
- `mupen64plus-video-gliden64`, `mupen64plus-video-glide64mk2`, `mupen64plus-video-gln64`, `mupen64plus-video-rice`, `mupen64plus-video-angrylion-plus`, `mupen64plus-video-parallel`: opções de vídeo.
- `mupen64plus-audio-android`: áudio Android.
- `mupen64plus-input-android` e `mupen64plus-input-raphnet`: entrada.
- `ae-bridge`: integração entre o frontend Android, a janela nativa e a extensão de vídeo.
- `miniupnp-bridge`: integração adicional de rede.

No Gradle estudado, o app usa Java 17 e declara ABIs `armeabi-v7a`, `arm64-v8a`, `x86` e `x86_64`. A configuração nativa Android do Core inclui definições como `-DANDROID`, `-DWITHOUT_SDL=1` e `-DUSE_GLES=1`.

**Atenção:** a presença do plugin `video-parallel` no repositório não comprova que a configuração Android do AE use Vulkan, nem que esse caminho seja compatível ou mais rápido na Mali-G52. Isso precisa ser verificado no build e no dispositivo.

## 3. Inicialização do Core e conexão dos plugins

Arquivos principais:

- [CoreInterface.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreInterface.java)
- [CoreService.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreService.java)
- [ae_bridge.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/ae-bridge/src/ae_bridge.cpp)

Fluxo observado:

1. O frontend carrega `ae-bridge` primeiro, para preparar a integração JNI e o attach/detach de threads.
2. O Core é inicializado por meio da API Mupen64Plus (`CoreStartup`).
3. O frontend configura diretórios de dados/cache e callbacks de vídeo.
4. A ROM é aberta com `CoreDoCommand(M64CMD_ROM_OPEN)`.
5. Os plugins são carregados, identificados com `PluginGetVersion`, inicializados com `PluginStartup` e conectados por `CoreAttachPlugin`.
6. A sequência de conexão documentada no fluxo estudado é vídeo, áudio, input e RSP.
7. Com a ROM aberta e os plugins conectados, o frontend inicia a execução com `M64CMD_EXECUTE`.

O Core e os plugins precisam estar inicializados na ordem correta. Não basta copiar uma biblioteca `.so`: ela depende da API, dos símbolos, da configuração, da superfície de vídeo e do ciclo de vida esperado pelo frontend.

## 4. Caminho de vídeo: EGL, Surface e apresentação de frames

Arquivos relevantes:

- [PixelBuffer.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/opengl/PixelBuffer.java)
- [CoreService.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreService.java)
- [ae_bridge.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/ae-bridge/src/ae_bridge.cpp)

Pipeline observado, em termos gerais:

1. O frontend cria um `PixelBuffer` e uma `Surface` Android.
2. A `Surface` é entregue à camada nativa por `setNativeWindow(surface)`.
3. O bridge prepara EGL: escolhe configuração, cria contexto e janela EGL, e torna o contexto atual.
4. O plugin gráfico renderiza o frame.
5. A extensão de vídeo chama `VidExtFuncGLSwapBuf()`, que encaminha a apresentação para `eglSwapBuffers()` quando a superfície/contexto estão válidos.
6. O `SurfaceTexture` recebe os frames e a thread de renderização do frontend atualiza a textura com `updateTexImage()` e desenha a imagem na tela.

O bridge tenta atender ao perfil/contexto solicitado e possui caminhos alternativos, inclusive fallback para GLES 2.0 em determinadas falhas. Portanto, não é correto resumir o AE como “sempre usa GLES2”: o contexto efetivo depende do caminho de inicialização e dos recursos disponíveis.

### Implicação para o nosso projeto

Uma tela preta pode ocorrer mesmo com a CPU executando instruções se o pipeline de apresentação não estiver completo. É necessário diagnosticar separadamente:

- ROM aberta e Core em execução;
- plugin GFX inicializado e conectado;
- contexto EGL válido e atual na thread correta;
- superfície nativa válida;
- chamadas de apresentação de frame;
- consumo dos frames pela camada Android e desenho na tela.

Logs de CPU, por si só, não comprovam que a imagem está sendo renderizada ou apresentada.

## 5. GLideN64 e compatibilidade OpenGL

Arquivos estudados:

- [CMakeLists.txt](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-video-gliden64/upstream/src/CMakeLists.txt)
- [MupenPlusPluginAPI.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-video-gliden64/upstream/src/MupenPlusPluginAPI.cpp)
- [MupenPlusAPIImpl.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-video-gliden64/upstream/src/mupenplus/MupenPlusAPIImpl.cpp)
- [mupen64plus_DisplayWindow.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-video-gliden64/upstream/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp)
- [opengl_ContextImpl.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-video-gliden64/upstream/src/Graphics/OpenGLContext/opengl_ContextImpl.cpp)
- [opengl_GLInfo.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-video-gliden64/upstream/src/Graphics/OpenGLContext/opengl_GLInfo.cpp)

Pontos identificados:

- A implementação de janela do GLideN64 solicita um contexto OpenGL 3.3 Core em um dos caminhos.
- O bridge do AE tenta criar o contexto solicitado e pode recorrer a atributos GLES 2.0 quando necessário.
- O GLideN64 detecta versão, renderer, fabricante e extensões.
- O código possui caminhos condicionais para GLES2: adapta formatos e alvos de framebuffer, desativa recursos não disponíveis e pode usar uma forma alternativa de desenho.
- O caminho GLES2 pode recorrer a `glReadPixels`; isso pode ter custo de desempenho e precisa ser medido.
- O plugin usa caminhos de desenho diferentes conforme os recursos detectados.

**Conclusão prudente:** a compatibilidade não deve ser inferida apenas pelo nome do plugin. Precisamos identificar o contexto realmente criado no aparelho e testar as funções/extensões que o renderer exige. Não foi concluída uma validação prática do GLideN64 na Mali-G52 MC2.

## 6. RSP HLE: função e integração

Arquivos:

- [RSP HLE hle.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-rsp-hle/upstream/src/hle.c)
- [RSP HLE plugin.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-rsp-hle/upstream/src/plugin.c)
- [Core rsp_core.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/rsp_core.c)

O RSP HLE reconhece microcodes e executa rotinas de alto nível em C, em vez de interpretar individualmente todas as instruções do coprocessador RSP. Há implementações para tarefas gráficas, áudio e formatos específicos. O código estudado mantém cache limitado de identificações de microcode e prevê fallback conforme configuração quando um microcode não é reconhecido.

Fluxo gráfico resumido:

`Core agenda tarefa RSP → rsp.doRspCycles() → RSP HLE identifica microcode → HleProcessDlistList() → callback do Core → gfx.processDList() → plugin de vídeo interpreta a display list → EGL apresenta o frame`

O RSP HLE não desenha diretamente os pixels. Ele processa/encaminha a tarefa gráfica ao Core e ao plugin de vídeo.

O caminho de áudio tem tratamento próprio para tarefas de áudio e integração com a API/plugin de áudio. O áudio Android é um módulo separado no projeto AE; não confundir o processamento de microcode de áudio com o dispositivo final de saída de áudio.

**Compatibilidade:** HLE pode reduzir trabalho em determinados jogos, mas não garante compatibilidade universal. Um microcode não reconhecido ou uma implementação incompleta pode exigir outro RSP ou fallback.

## 7. CPU MIPS e Dynarec ARM64

Arquivos:

- [r4300_core.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/r4300_core.c)
- [new_dynarec.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/new_dynarec/new_dynarec.c)
- [assem_arm64.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/new_dynarec/arm64/assem_arm64.c)
- [linkage_arm64.S](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/new_dynarec/arm64/linkage_arm64.S)
- [mupen64plus-core.mk](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/mupen64plus-core.mk)

Na configuração ARM64 estudada, o build ativa definições como `-DDYNAREC` e `-DNEW_DYNAREC=4`, além de incluir o assembly ARM64 de ligação.

O Dynarec é parte do Core R4300, não um plugin independente. Ele recompila blocos de instruções MIPS em código nativo ARM64, mantém cache de blocos e gerencia invalidação quando o código de origem é alterado.

### Seleção em tempo de execução

O fato de compilar com suporte Dynarec não garante que ele será usado. O modo é escolhido pela configuração `Core/R4300Emulator`:

- `0`: interpretador puro;
- `1`: cached interpreter;
- `2` ou superior: Dynarec, se o binário tiver sido compilado com suporte.

No AE, o frontend sincroniza a configuração antes da inicialização do Core. O código também contém uma regra que pode forçar interpreter em determinada configuração relacionada a FBInfo. Assim, para confirmar o modo ativo, é preciso verificar o valor efetivo da configuração e os logs do runtime, não apenas as flags de compilação.

Arquivos de referência adicionais:

- [NativeConfigFiles.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/NativeConfigFiles.java)
- [main.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/main/main.c)

## 8. Cache de código do Dynarec ARM64

No caminho ARM64 estudado:

- `TARGET_SIZE_2` vale 25, resultando em (2^{25}) bytes, ou 32 MiB.
- O Core declara `extra_memory[33554432]`, alinhado a 4096 bytes.
- O buffer de código gerado usa essa região como cache executável no caminho padrão estudado.
- O gerenciador trata o espaço como buffer circular dividido em oito segmentos de aproximadamente 4 MiB.
- O ponteiro de saída avança pela região e retorna ao início quando necessário.
- Um mecanismo de expiração limpa segmentos antigos antes que sejam reutilizados.
- O código mantém metadados/tabelas de blocos e snapshots para verificar se o código MIPS original mudou.

A documentação do Dynarec descreve o princípio do buffer circular e a expiração de blocos antigos. O mecanismo não significa que todos os blocos são recompilados a cada escrita: há invalidação, verificação e possibilidade de recuperar blocos quando o código original continua idêntico.

## 9. Permissões de memória executável e risco a testar no Android

No caminho padrão observado em `new_dynarec.c`, o código chama:

```c
mprotect((u_char *)g_dev.r4300.extra_memory,
         1 << TARGET_SIZE_2,
         PROT_READ | PROT_WRITE | PROT_EXEC);
base_addr = g_dev.r4300.extra_memory;
base_addr_rx = base_addr;
```

A chamada pede permissões de leitura, escrita e execução (RWX). No trecho analisado, o retorno de `mprotect()` não é verificado antes de o código continuar configurando o endereço base. Portanto, uma falha nessa chamada pode não ser detectada ali.

O código-fonte também contém alternativas condicionais:

- cache dinâmico com `mmap()` RWX;
- cache duplo com mapeamentos separados para escrita e execução (RW/RX), usando memória compartilhada.

Essas alternativas não devem ser tratadas como fallback automático comprovado para a configuração padrão. É necessário verificar as macros efetivamente usadas no build.

### Plano de validação Android

1. Registrar o resultado e o código de erro de `mprotect()`.
2. Confirmar que o endereço e o tamanho estão corretamente alinhados.
3. Verificar o tamanho de página do aparelho com `adb shell getconf PAGE_SIZE`.
4. Testar em dispositivo real se a política de memória permite o modo escolhido.
5. Se o Dynarec não puder ser ativado com segurança, selecionar explicitamente um interpretador como fallback, em vez de continuar supondo que a região é executável.
6. Registrar no log o modo realmente selecionado e a razão do fallback.

O alinhamento de 4096 bytes encontrado no código não prova compatibilidade com dispositivos que usam páginas de 16 KiB. Também não há evidência aqui de que o Poco C85 use 4 KiB ou 16 KiB: isso precisa ser medido no aparelho.

**Não concluir antecipadamente que Android 16 bloqueia o Dynarec ou que o Poco C85 falhará.** São pontos de teste, não resultados confirmados.

## 10. Threads, execução, pausa e parada

Arquivos:

- [CoreService.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreService.java)
- [CoreInterface.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreInterface.java)
- [frontend.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/api/frontend.c)
- [main.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/main/main.c)
- [device.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/device.c)
- [interrupt.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/interrupt.c)

No fluxo AE estudado, o `CoreService` cria uma `HandlerThread` chamada `ServiceStartArguments`. O handler prepara o Core, abre a ROM e conecta os plugins. Em seguida, `emuStart()` envia `M64CMD_EXECUTE`; essa chamada bloqueia até a emulação parar.

O caminho nativo segue, em linhas gerais:

`M64CMD_EXECUTE → main_run() → poweron_device() → run_device() → run_r4300() → Dynarec ou interpretador`

A execução principal permanece no contexto da thread que chamou `M64CMD_EXECUTE` nesse fluxo observado. Isso não significa que áudio, vídeo ou outros componentes não possam possuir threads próprias.

### Pausa

A pausa altera o estado do Core e ativa um loop de pausa que continua verificando entradas e pode atualizar a apresentação. A pausa não equivale a destruir e recriar o Dynarec.

### Parada

`M64CMD_STOP` sinaliza a parada do dispositivo. O Core verifica a flag de parada em pontos de interrupção. No caminho Dynarec ARM64, o assembly de ligação verifica a flag e desvia para a rotina de saída, restaura o contexto e retorna ao código C. Depois disso, o caminho de execução chama a limpeza do Dynarec.

Fluxo resumido:

`M64CMD_STOP → stop_device() marca flag → próxima interrupção verifica a flag → new_dyna_stop restaura contexto → retorna de new_dyna_start() → new_dynarec_cleanup()`

A parada é assíncrona: o pedido não significa que a CPU emulada foi interrompida instantaneamente no meio de qualquer instrução.

## 11. Invalidação e sincronização do código ARM64

No assembly ARM64 estudado, os stubs de invalidação chamam rotinas como `invalidate_addr()`. O caminho de código modificado também usa verificação (`verify_code()` / `verify_dirty()`) para comparar o conteúdo original com a memória atual.

A sincronização de caches de instrução/dados inclui operações ARM64 como:

- `dc civac`
- `ic ivau`
- `dsb ish`
- `isb`

Essas operações são importantes porque código recém-gerado ou modificado precisa estar visível corretamente para o processador ao ser executado. Não se deve remover essas operações sem entender o protocolo de geração e invalidação.

## 12. Áudio

No AE, o áudio Android é um componente separado do Core e do plugin gráfico. A API de áudio inclui callbacks como `AiDacrateChanged`, `AiLenChanged`, `InitiateAudio` e `ProcessAList`. O RSP pode processar microcodes de áudio, enquanto o módulo de áudio trata a saída e integração com Android.

A investigação identificou Oboe e SoundTouch na pilha de áudio do projeto de referência. Isso não comprova que uma integração parcial desses componentes, sem a API e o ciclo de vida corretos, funcione no nosso aplicativo.

## 13. Estratégia de diagnóstico para o nosso emulador

Separar os subsistemas e registrar resultados independentes:

| Subsistema | Estado observado no M64plus | Próxima evidência necessária |
|---|---|---|
| ROM/loader | Ainda não implementado | ROM de teste validada e boot reproduzível |
| CPU/Core | Interpretador MIPS inicial e integração com barramento | testes de instruções e execução de código de boot |
| Dynarec | Não comprovado como implementado no M64plus | geração/execução ARM64 e fallback medido |
| RSP | Interpretador escalar parcial e COP2 parcial | cobertura vetorial, tarefas e sincronização |
| GFX/RDP | Não implementado como pipeline de jogos | display lists, comandos RDP e framebuffer |
| EGL/Surface | Triângulo de diagnóstico GLES2 observado | contexto, renderer, ciclo de vida e apresentação contínua |
| Áudio | Não implementado | tarefas RSP de áudio e saída Android |
| Android UI | Tela de diagnóstico | apresentação de frames gerados pelo emulador |

Não usar “o PC avançou” nem o triângulo de diagnóstico como prova de que um jogo está funcionando. CPU, RSP, RDP, áudio e apresentação precisam ser validados separadamente.

## 14. Próximos testes prioritários

1. Completar COP2 com testes de referência para operações vetoriais, flags VCO/VCC/VCE e accumulator; não assumir que a implementação parcial atual reproduz todos os detalhes do RSP real.
2. Implementar loads/stores vetoriais e ampliar instruções escalares do RSP.
3. Revisar branches, delay slots, BREAK e comportamento de SP_PC/SP_STATUS com casos de teste de borda.
4. Modelar a sincronização CPU↔RSP, interrupções e DMA com temporização/orçamento de ciclos, em vez de tratar a execução como cópia instantânea.
5. Implementar PI e mapeamento de cartucho; criar carregador de ROM de teste e validar o caminho de boot.
6. Só depois de tarefas gráficas RSP confiáveis, implementar RDP e um caminho de framebuffer.
7. Validar EGL/Surface e renderer N64 separadamente; o triângulo GLES2 atual é somente diagnóstico.
8. Implementar AI/áudio e os demais dispositivos, depois integrar e medir o conjunto no Poco C85.

## 15. O que ainda não está comprovado

- Que GLideN64 funcione corretamente na Mali-G52 MC2 do Poco C85.
- Que o caminho de vídeo Android do AE use Vulkan.
- Que o aparelho use páginas de memória de 4 KiB ou 16 KiB.
- Que permissões RWX sejam permitidas ou negadas no Android 16 desse aparelho.
- Que o fallback de memória executável esteja ativo no build padrão.
- Que o RSP parcial atual cubra as instruções vetoriais ou microcodes exigidos por jogos comerciais.
- Que CPU/RSP estejam sincronizados em ciclos como o hardware real.
- Que exista boot de ROM, RDP, áudio ou compatibilidade com jogos.
- Que copiar bibliotecas nativas do AE isoladamente seja suficiente para obter um emulador funcional.
- Que o renderer de diagnóstico GLES2 seja um renderer Nintendo 64.

## 16. Referências principais

- [Mupen64Plus AE — repositório](https://github.com/mupen64plus-ae/mupen64plus-ae)
- [CoreInterface.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreInterface.java)
- [CoreService.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/CoreService.java)
- [NativeConfigFiles.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/jni/NativeConfigFiles.java)
- [ae_bridge.cpp](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/ae-bridge/src/ae_bridge.cpp)
- [PixelBuffer.java](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/app/src/main/java/paulscode/android/mupen64plusae/opengl/PixelBuffer.java)
- [RSP HLE — hle.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-rsp-hle/upstream/src/hle.c)
- [RSP HLE — plugin.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-rsp-hle/upstream/src/plugin.c)
- [R4300 core](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/r4300_core.c)
- [Dynarec ARM64 — new_dynarec.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/new_dynarec/new_dynarec.c)
- [Dynarec ARM64 — assem_arm64.c](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/new_dynarec/arm64/assem_arm64.c)
- [Dynarec ARM64 — linkage_arm64.S](https://github.com/mupen64plus-ae/mupen64plus-ae/blob/master/mupen64plus-core/upstream/src/device/r4300/new_dynarec/arm64/linkage_arm64.S)
- [Android: suporte a páginas de 16 KB](https://developer.android.com/guide/practices/page-sizes)

---

## Registro de continuidade

Ao retomar este projeto em outra conversa:

1. Ler este arquivo antes de propor alterações.
2. Distinguir sempre fatos observados no código de referência, hipóteses e resultados de testes no aparelho.
3. Não modificar o repositório antigo `N64-Android`.
4. Não importar/copiar código do Mupen64Plus AE sem avaliar dependências, licença e integração.
5. Antes de modificar o código de M64plus, apresentar o plano e aguardar autorização explícita do usuário.
6. Atualizar este documento quando uma investigação ou teste produzir resultado verificável, registrando arquivo, commit, dispositivo, versão Android e logs relevantes.


## 17. Barramento físico e dispositivos iniciais (M64plus)

Arquivos principais: `app/src/main/cpp/n64/bus.h` e `bus.cpp`.

| Região | Endereço físico | Tamanho / observação | Estado observado |
|---|---:|---|---|
| RDRAM | `0x00000000` | 8 MiB | Implementada, big-endian |
| SP DMEM | `0x04000000` | 4 KiB | Implementada |
| SP IMEM | `0x04001000` | 4 KiB | Implementada |
| MI registers | `0x04300000` | 0x10 bytes | MI_MODE, MI_VERSION, MI_INTR, MI_INTR_MASK iniciais |
| SP registers | `0x04040000` | 0x20 bytes | SP_MEM_ADDR, SP_DRAM_ADDR, READ/WRITE_LENGTH, STATUS, DMA_FULL/BUSY e SEMAPHORE |
| SP_PC | `0x04080000` | 4 bytes | Registrador de PC do RSP |
| Demais dispositivos | — | — | Não mapeados neste estágio |

A classe `Bus` oferece acessos big-endian de 8, 16, 32 e 64 bits, valida que cada acesso permaneça dentro de uma região e rejeita acessos MMIO que não sejam de 32 bits alinhados. A CPU principal acessa o barramento compartilhado por meio de `N64Machine`; os testes de integração executam instruções a partir da IMEM e leem/escrevem DMEM e RDRAM.

O DMA do SP suporta comprimento arredondado em blocos de 8 bytes, contagem de linhas, skip de RDRAM e alternância/wrap entre DMEM e IMEM. **Limitação:** a transferência é síncrona e imediata; fila, latência, concorrência e temporização por ciclos ainda não estão modeladas. O MI implementa apenas registradores iniciais e máscara/status de interrupções; não equivale ao conjunto completo de dispositivos do console.

Testes host em `tests/bus_tests.cpp` e autotestes JNI cobrem decodificação, big-endian, independência DMEM/IMEM, DMA, linhas/skip, wrap, limites, MI, SP_STATUS, SP_PC e interrupções iniciais.


## 18. CPU/RSP e máquina compartilhada (M64plus)

Arquivos principais:
- `app/src/main/cpp/n64/machine.h` / `machine.cpp`: classe `N64Machine`, dona do barramento, CPU principal e RSP.
- `app/src/main/cpp/n64/rsp_cpu.h` / `rsp_cpu.cpp`: interpretador RSP escalar e unidade vetorial parcial.
- `tests/rsp_cpu_tests.cpp`, `tests/machine_tests.cpp` e `app/src/main/cpp/rsp-machine-jni.cpp`: testes de instruções e integração.

### RSP escalar implementado parcialmente

O interpretador atual contém, entre outras instruções, operações aritméticas/lógicas escalares, imediatas, loads/stores escalares para DMEM, saltos/desvios com delay slot e BREAK. A execução do RSP é limitada por orçamento de instruções em `runRsp()`; isso é uma ferramenta inicial de teste, não um agendador fiel ao relógio do N64.

### COP2 implementado parcialmente

O código atual implementa:
- Movimentação: MFC2, MTC2, CFC2 e CTC2.
- Aritmética: VADD, VSUB, VADDC e VSUBC.
- Lógica: VAND, VNAND, VOR, VNOR, VXOR e VNXOR.
- Registradores vetoriais de 8 lanes de 16 bits, accumulator por lane e estado inicial de VCO, VCC e VCE.
- Mapeamento de elemento para seleção de lanes e acesso aos bytes dos registradores vetoriais.

**Não declarar COP2 completo.** Permanecem ausentes grande parte das operações vetoriais do RSP, loads/stores vetoriais, comportamento completo de flags/accumulator e validação diferencial contra casos de referência. Pipeline hazards e temporização também não estão implementados. Os testes atuais confirmam apenas os casos explicitamente cobertos.

### Integração com SP e CPU

`N64Machine::stepRsp()` verifica SP_HALT, sincroniza o PC do interpretador com SP_PC, executa uma instrução e atualiza SP_PC. Ao executar BREAK, sinaliza SP_HALT/SP_BROKE e, se habilitado, a interrupção do SP no MI. Os testes JNI cobrem:
- inicialização e execução do RSP a partir de IMEM;
- BREAK e sinalização de interrupção;
- CPU iniciando o RSP via registradores SP;
- CPU lendo o resultado escrito pelo RSP em DMEM;
- uma tarefa vetorial mínima com VADD, seguida de BREAK.

O teste integrado confirma um cenário mínimo, não execução de microcodes comerciais, display lists completas ou jogos.

### Próxima implementação recomendada

A próxima etapa de código deve ser estreita e verificável: adicionar instruções de transferência vetorial (por exemplo, LBV/LSV/LLV/LDV e SBV/SSV/SLV/SDV, depois os formatos de load/store restantes), cada uma com testes de endianness, alinhamento, elemento e wrap de endereço. Em paralelo, expandir COP2 em pequenos grupos com testes de flags e accumulator. Não iniciar RDP/renderização de jogos antes de o caminho de tarefas gráficas do RSP estar suficientemente testado.
