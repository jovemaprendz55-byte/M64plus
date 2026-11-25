# Documentação técnica — M64plus

> **Objetivo:** registrar a pesquisa de arquitetura do Mupen64Plus AE como referência técnica para o desenvolvimento de um emulador Nintendo 64 para Android, com atenção ao Android 16 e ao Poco C85 (Unisoc T7250 / Mali-G52 MC2).
>
> **Estado:** documento de pesquisa. Os pontos abaixo descrevem o código de referência estudado e hipóteses/testes propostos. Não significam que essas partes já estejam implementadas neste repositório.
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

| Subsistema | O que comprovar | Evidência útil |
|---|---|---|
| ROM/loader | ROM válida, tamanho e inicialização | logs de leitura e validação |
| CPU/Core | loop de execução ativo | PC, ciclos, estado e modo de CPU |
| Dynarec | geração e execução de blocos ARM64 | logs de inicialização, cache e falhas de memória |
| RSP | tarefa reconhecida e concluída | tipo de tarefa, microcode e callbacks |
| GFX | display lists recebidas e processadas | contadores de listas e erros do plugin |
| EGL/Surface | contexto e superfície válidos | versão GL, renderer, estado EGL e thread |
| Áudio | tarefas e buffers entregues | contadores de amostras/buffers e erros |
| Android UI | frames recebidos e desenhados | callbacks de frame e estado da Surface |

Não usar “o PC avançou” como prova de que o jogo está funcionando. CPU, RSP, vídeo, áudio e apresentação precisam ser validados separadamente.

## 14. Próximos testes prioritários

1. Confirmar no Poco C85 a ABI efetiva do processo e o tamanho de página do sistema.
2. Criar um teste mínimo de EGL que registre versão, renderer, extensões e formato de superfície realmente disponíveis.
3. Validar o ciclo de vida de `Surface`, EGL context e thread de renderização.
4. Medir se o Dynarec consegue reservar/proteger memória executável; registrar falhas e fornecer fallback explícito para cached interpreter.
5. Validar CPU com ROM de teste e comparar Dynarec com interpretador.
6. Validar RSP HLE com tarefas de áudio e gráficos, incluindo comportamento quando o microcode não é reconhecido.
7. Integrar um plugin gráfico apenas depois que a API do Core, a superfície e o contexto EGL estiverem corretos.
8. Testar áudio em separado e depois em conjunto com vídeo.
9. Medir desempenho e estabilidade por jogo; não assumir que uma configuração rápida em um título será ideal em todos.

## 15. O que ainda não está comprovado

- Que GLideN64 funcione corretamente na Mali-G52 MC2 do Poco C85.
- Que o caminho de vídeo Android do AE use Vulkan.
- Que o aparelho use páginas de memória de 4 KiB ou 16 KiB.
- Que permissões RWX sejam permitidas ou negadas no Android 16 desse aparelho.
- Que o fallback de memória executável esteja ativo no build padrão.
- Que RSP HLE cubra todos os microcodes/jogos desejados.
- Que copiar bibliotecas nativas do AE isoladamente seja suficiente para obter um emulador funcional.
- Que a arquitetura pesquisada já esteja implementada no repositório M64plus.

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


## 17. Barramento físico inicial (M64plus)

Commit inicial desta etapa: o barramento foi separado da RDRAM e possui decodificação explícita das regiões:

| Região | Endereço físico | Tamanho | Estado |
|---|---:|---:|---|
| RDRAM | `0x00000000` | 8 MiB | Implementada, big-endian |
| SP DMEM | `0x04000000` | 4 KiB | Implementada como memória independente |
| SP IMEM | `0x04001000` | 4 KiB | Implementada como memória independente |
| Demais endereços | — | — | Ainda não mapeados; leitura/escrita retorna falha |

A classe `Bus` oferece acessos de 8, 16, 32 e 64 bits, valida que cada acesso permaneça dentro de uma única região e expõe a RDRAM para inspeção/testes. O autoteste JNI valida decodificação, big-endian, independência DMEM/IMEM, limites e reset.

**Limite desta entrega:** a CPU ainda executa diretamente sobre `Rdram`; esta classe é uma fundação isolada e ainda não substitui o caminho de memória do processador. Também não existem ainda registradores MMIO do SP, MI, VI, AI, PI, RI ou SI, interrupções por dispositivos, DMA ou mapeamento de cartucho. O próximo passo é implementar os blocos de registradores e então migrar a CPU para o barramento sem quebrar os testes existentes.


## 18. Investigação do rasterizador RDP — referências e escopo (25/11/2026)

### 18.1 Referências consultadas

- [Nintendo Ultra64 RDP Command Summary, versão 2.0 (SGI, 11/04/1996)](https://hcs64.com/files/RDP_COMMANDS.pdf), especialmente páginas 12–24: composição dos comandos de triângulo, coeficientes de borda, shade, texture e Z.
- [Nintendo 64 Programming Manual — capítulo 12, RDP Programming](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-01.html): blocos lógicos do pipeline, modos de ciclo e funções de rasterização, textura, filtro, combiner, blender e interface de memória.

### 18.2 Fatos extraídos das referências

- Os comandos de triângulo são compostos por blocos concatenados nesta ordem: edge, shade opcional, texture opcional e Z opcional. O resumo de comandos especifica quatro palavras de 64 bits para edge; oito para shade; oito para texture; duas para Z.
- O bloco edge carrega YH/YM/YL em coordenadas subpixel 11.2, além de X e inclinações inversas das três bordas. O documento também descreve que os valores X são amostrados em pontos específicos da malha de scanlines; portanto, não basta tratar o triângulo como três vértices geométricos comuns.
- O rasterizador produz coordenadas e atributos como RGBA, Z, S/W, T/W, 1/W, L e cobertura.
- O manual divide o pipeline em seis blocos lógicos: RS (rasterização), TX (texturização), TF (filtro), CC (combinação de cor), BL (blending) e MI (acesso à memória de pixels). Os modos 1-cycle, 2-cycle, copy e fill alteram a configuração do pipeline.

### 18.3 Comparação com o estado atual do repositório

- `RdpCommandDecoder` já separa o layout dos oito opcodes de triângulo e decodifica os blocos edge, shade, texture e Z, preservando os coeficientes e os dados brutos.
- `RdpCommandProcessor` retém o último triângulo e consome o pacote completo, mas reporta `Unsupported`; não há ainda edge walker, geração de cobertura ou escrita de pixels para triângulos.
- `FILL_RECTANGLE` é um caminho separado e limitado ao ciclo FILL. Ele não implementa o pipeline geral RS/TX/TF/CC/BL/MI.

### 18.4 Sequência recomendada antes de implementar

1. Construir vetores de referência para edge walking: triângulo left-major e right-major, vértice horizontal, altura de uma scanline, coordenadas subpixel e limites YH/YM/YL.
2. Definir e testar a regra de cobertura por pixel e as regras de inclusão/exclusão das bordas, comparando resultados com uma implementação de referência independente ou hardware.
3. Só depois adicionar interpolação shade e Z, com vetores que validem precisão, sinais, frações e acumulação.
4. Tratar textura, filtragem, combiner, blender, coverage e interface de memória como etapas separadas; não declarar suporte geral de triângulos a partir de um teste geométrico isolado.

**Limitação e decisão:** a documentação consultada descreve formatos e blocos, mas não constitui por si só uma suíte completa de conformidade bit a bit. Não foi iniciada implementação do rasterizador nesta etapa. Antes de alterar código de produção, é necessário obter vetores de referência reproduzíveis para a cobertura e o edge walker.

### 18.5 Validação documental no CI

A revisão documental desta investigação foi registrada no commit `8cddc068488e9f7b2892cedcaabe5fef07fb4611`. A execução do GitHub Actions nº 754 terminou com sucesso: `native-host-tests` e `assemble-debug` passaram. Como esse commit alterou apenas documentação, esse resultado confirma que o repositório continuou compilando e que os testes existentes passaram; não valida um rasterizador, que ainda não foi implementado.

Execução: https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45574781578

### 18.6 Descoberta de uma referência de conformidade independente

Foi examinada a documentação pública do projeto [Themaister/parallel-rdp](https://github.com/Themaister/parallel-rdp):

- O README descreve um conjunto `rdp-conformance` com aproximadamente 150 testes de fuzzing e comparação bit a bit contra Angrylion-Plus.
- O projeto informa que usa um gerador próprio de comandos RDP de nível mais alto e que Angrylion-Plus é a referência para validar os resultados.
- O repositório declara licença MIT para o código do paraLLEl-RDP. Isso **não** concede automaticamente direitos sobre Angrylion-Plus nem sobre outros componentes referenciados; cada dependência precisa de análise de licença separada.
- A arquitetura principal do paraLLEl-RDP é baseada em Vulkan compute e não é apropriada para copiar diretamente para o backend Android/C++ atual. O valor imediato aqui é metodológico: comparar saídas com um oráculo independente, em vez de validar o rasterizador apenas contra seus próprios testes.

**Próximo passo seguro:** estudar o formato e a organização dos testes de conformidade, sem copiar código. Para o M64plus, priorizar pequenos vetores próprios de comandos RDP e framebuffers esperados, gerados por uma referência cuja licença e condições de uso tenham sido verificadas. Começar por triângulos planos e cobertura; só depois avançar para shade, Z, textura e blending. Nenhuma alteração de código de produção foi feita nesta investigação.

Referências: [README do paraLLEl-RDP](https://github.com/Themaister/parallel-rdp) · [LICENSE do paraLLEl-RDP](https://github.com/Themaister/parallel-rdp/blob/master/LICENSE) · [Artigo técnico sobre cobertura no RDP](https://emudev.org/2021/09/21/Angrylion_RDP_Comments).

### 18.7 Como o harness de conformidade organiza os testes

Leitura estática do arquivo público `rdp_conformance.cpp` e de `conformance_utils.hpp` do paraLLEl-RDP (sem copiar ou incorporar código):

- O mesmo `CommandBuilder` alimenta dois drivers: a implementação de referência Angrylion e o backend paraLLEl-RDP. Isso reduz o risco de os dois lados receberem comandos diferentes.
- A memória é limpa antes de cada caso; em casos específicos, RDRAM e TMEM são inicializadas com dados pseudoaleatórios. O gerador usa uma semente fixa (1337), tornando a sequência reproduzível.
- A validação compara a RDRAM inteira, a RDRAM oculta e a imagem final. A comparação de imagem percorre os pixels e informa a primeira divergência de RGB com coordenadas.
- Os casos são organizados em suítes nomeadas e selecionáveis pela linha de comando. As variantes combinam opções como ciclo 1/2, AA, profundidade, scissor, interlace, formatos de textura, TLUT, filtragem e conversão.
- O arquivo inclui geradores de primitivas com posições, Z, W, UV e cores variáveis; há também opções para forçar geometrias invertidas e retângulos de preenchimento/textura.

**O que podemos aproveitar conceitualmente no M64plus:**

1. Usar uma entrada serializada idêntica para o processador sob teste e para o oráculo externo.
2. Fixar sementes e registrar cada entrada que falhar, para permitir reprodução exata.
3. Comparar framebuffer byte a byte e apontar o primeiro endereço/pixel divergente.
4. Separar suítes pequenas por recurso: geometria/cobertura, shade, Z, textura/TMEM, combiner e blender.
5. Manter os testes determinísticos de casos-limite separados dos testes aleatórios; fuzzing amplia cobertura, mas não substitui vetores mínimos legíveis.

**Limite importante:** o harness observado compara duas implementações completas (Angrylion e GPU/Vulkan), não fornece automaticamente vetores dourados independentes prontos para o nosso decodificador. Para o M64plus, o primeiro marco deve ser um formato mínimo de caso de teste (estado inicial + fluxo de comandos + RDRAM inicial + framebuffer esperado), antes de qualquer edge walker. Isso é uma proposta de desenho; nenhum código de produção foi alterado.

Referências de leitura: [rdp_conformance.cpp](https://github.com/Themaister/parallel-rdp/blob/master/rdp_conformance.cpp) · [conformance_utils.hpp](https://github.com/Themaister/parallel-rdp/blob/master/conformance_utils.hpp).
