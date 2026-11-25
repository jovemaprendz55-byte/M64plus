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


### 18.8 Formato mínimo proposto para vetores RDP do M64plus

Antes de criar um executor de testes, cada caso deve ser capaz de descrever a entrada e o resultado esperado sem depender do estado deixado por outro caso. O formato inicial pode ser JSON legível, versionado e independente da implementação:

```json
{
  "schema_version": 1,
  "case_id": "fill-rectangle-rgba16-basic",
  "feature": "fill_rectangle",
  "initial_state": {
    "rdram_size": 8388608,
    "color_image": {"format": "rgba", "size": 16, "width": 8, "address": 4096},
    "scissor": {"left": 0, "top": 0, "right": 8, "bottom": 8},
    "fill_color": "0xF80107C1",
    "other_modes": {"cycle_type": "fill"}
  },
  "commands": ["0x..."],
  "expected": {
    "status": "success",
    "memory_regions": [
      {"address": 4096, "hex": "F80107C1"}
    ]
  }
}
```

O trecho é ilustrativo: os campos e a codificação final dos comandos precisam acompanhar as estruturas reais do M64plus; não é ainda um arquivo de teste executável. Para triângulos, o caso deverá declarar os words exatos do comando e os bytes esperados do framebuffer, além de inicializar explicitamente todos os estados que influenciam o resultado.

**Regras para o primeiro executor:**

1. Recriar o estado inicial para cada caso, sem compartilhar RDRAM, registradores ou TMEM entre testes.
2. Validar versão do schema, tamanhos, endereços e comprimentos antes de executar; entradas inválidas devem falhar claramente, sem escrita parcial.
3. Registrar o identificador do caso, semente (quando houver aleatoriedade), fluxo bruto de comandos e configuração do teste.
4. Comparar somente regiões explicitamente declaradas na primeira versão; ampliar depois para framebuffer completo, RDRAM oculta e TMEM quando esses caminhos estiverem implementados.
5. Ao divergir, informar primeiro endereço ou pixel, valor esperado e observado, formato e coordenadas quando aplicável.
6. Manter vetores determinísticos e pequenos como testes obrigatórios. Casos aleatórios devem ser reproduzíveis por semente e exportar o caso que falhou para um vetor fixo.
7. Não usar a saída do próprio M64plus como resultado esperado. Cada vetor dourado precisa indicar a origem verificável (hardware, documentação com resultado inequívoco ou implementação de referência licenciada e identificada).

**Ordem sugerida:** começar pelos comandos já suportados e testáveis, como FILL_RECTANGLE, para validar serialização, estado inicial, execução e comparação de memória. Em seguida, criar vetores de decodificação de triângulos que validem apenas o pacote decodificado; isso não deve ser confundido com validação de cobertura ou pixels. Vetores de framebuffer para triângulos só entram quando houver um oráculo independente e licença/condições de uso verificadas.

**Critério de conclusão desta etapa:** pelo menos um vetor fixo deve ser carregado pelo executor, produzir o mesmo resultado em execuções repetidas e apontar uma divergência artificial injetada no endereço correto. Isso valida a infraestrutura de teste, não a precisão geral do RDP. Nenhum código de produção foi alterado nesta etapa.


### 18.9 Primeiro caso de referência extraído dos testes existentes

Foi revisado `tests/rdp_tests.cpp`, especialmente `testFillRectangleWritesRgba32PixelsAndHonorsScissor`. O teste já existente permite definir um primeiro fixture concreto para validar futuramente o formato descrito na seção 18.8. Este registro documenta a entrada e o resultado observados no teste atual; ainda não é um arquivo JSON consumido por um executor independente.

**Caso:** `fill-rectangle-rgba32-4x4-center`

- RDRAM: 8 MiB; framebuffer RGBA32 na base `0x3000`, largura de 4 pixels.
- Inicialização: 16 pixels com sentinela `0xAABBCCDD`.
- Comandos RDP, na ordem:
  - `0x3F18000300003000` — SET_COLOR_IMAGE, RGBA32, largura 4, endereço `0x3000`.
  - `0x3700000011223344` — SET_FILL_COLOR.
  - `0x2F30000000000000` — SET_OTHER_MODES, ciclo FILL.
  - `0x3600800800004004` — FILL_RECTANGLE, região de (1,1) a (2,2), inclusiva.
- Resultado esperado: apenas os pixels de índices lineares 5, 6, 9 e 10 mudam para `0x11223344`. Todos os demais permanecem `0xAABBCCDD`.

**Bytes esperados nas quatro posições alteradas (big-endian):**

| Endereço | Pixel (x,y) | Bytes |
|---|---|---|
| `0x3014` | (1,1) | `11 22 33 44` |
| `0x3018` | (2,1) | `11 22 33 44` |
| `0x3024` | (1,2) | `11 22 33 44` |
| `0x3028` | (2,2) | `11 22 33 44` |

Esse fixture deve ser transcrito para o schema versionado somente quando o executor for desenhado. A validação futura precisa comparar também os 12 pixels sentinela que não podem mudar; verificar somente os quatro pixels escritos não detectaria escritas indevidas fora do retângulo.

**Rastreabilidade:** o caso deriva de um teste nativo já presente no repositório e não de uma saída independente do hardware/oráculo. Portanto, é adequado para testar serialização e regressão do comportamento implementado, mas não deve ser rotulado como prova independente de conformidade do RDP. Nenhum código de produção ou teste executável foi alterado nesta etapa.


### 18.10 Vetor de clipping com seleção de campo (interlace)

Foi analisado o teste existente `testFillRectangleHonorsScissorFieldParity` em `tests/rdp_tests.cpp`. Ele executa o mesmo retângulo duas vezes, alternando o campo selecionado, e verifica todos os 16 pixels de um framebuffer 4×4.

**Estado e entrada:**

- Framebuffer RGBA32, largura 4, endereço `0x3800`; todos os 16 pixels começam em `0xAABBCCDD`.
- Cor de preenchimento: `0x10203040`; ciclo FILL.
- Scissor: X de 0 até 3 (borda direita exclusiva), Y de 0 até 4; seleção de campo habilitada.
- Retângulo de preenchimento cobre a área 4×4; o scissor reduz a largura efetiva para 3 pixels.
- Fluxo comum:
  - `0x3F18000300003800` — SET_COLOR_IMAGE.
  - `0x3700000010203040` — SET_FILL_COLOR.
  - `0x2F30000000000000` — SET_OTHER_MODES (FILL).
  - `0x3600C00C00000000` — FILL_RECTANGLE.
- Quarto comando (SET_SCISSOR) varia para selecionar cada campo:
  - Campo par: `0x2D0000000200C010`.
  - Campo ímpar: `0x2D0000000300C010`.

**Resultado esperado:**

| Campo selecionado | Linhas que recebem a cor | Pixels coloridos por linha |
|---|---|---|
| Par | y=0 e y=2 | x=0, 1 e 2 |
| Ímpar | y=1 e y=3 | x=0, 1 e 2 |

Todos os demais pixels devem permanecer em `0xAABBCCDD`. A coordenada x=3 deve permanecer intacta em todas as linhas, confirmando a borda direita exclusiva do scissor. Esse caso cobre simultaneamente seleção de campo e clipping horizontal, mas não substitui testes separados para cada borda do scissor.

**Rastreabilidade e limite:** o resultado é derivado de um teste de regressão interno existente, não de uma medição independente de hardware. É útil como fixture de regressão/serialização, não como oráculo externo de conformidade. Nenhum código de produção ou teste executável foi alterado.



### 18.11 Inventário dos casos de regressão de FILL_RECTANGLE

Foi feita uma leitura do conjunto atual de testes em `tests/rdp_tests.cpp`. O inventário abaixo organiza os casos por comportamento coberto, sem afirmar que eles formam uma referência externa de conformidade.

| Grupo | Casos existentes | O que verificam |
|---|---|---|
| Escrita básica | RGBA32 4×4; pixel único; RGBA16 | Escrita no formato e nos endereços esperados, inclusive retângulo degenerado. |
| Clipping | Quatro bordas do scissor; interseção vazia; coordenadas invertidas | Limites inclusivos do retângulo de comando, bordas exclusivas do scissor e rejeição de regiões inválidas. |
| Paridade | RGBA16 preserva paridade; seleção de campo par/ímpar | Índice do pixel/halfword e linhas habilitadas pelo campo selecionado. |
| Limites de memória | Último pixel RGBA32; último halfword RGBA16; overflow na primeira ou em linhas posteriores | Acesso no limite exato e rejeição de endereços fora da RDRAM. |
| Atomicidade | Casos de overflow e coordenadas fora do intervalo | Uma falha não pode deixar escrita parcial no framebuffer. |
| Estado obrigatório | Sem color image, fill color ou other modes | O comando não deve escrever quando falta estado necessário. |
| Modo/formato | Ciclo diferente de FILL; formatos de color image não suportados | Rejeição explícita dos caminhos ainda não implementados. |

**Observações para a próxima etapa de vetores:**

1. Os casos de RGBA32 e RGBA16 precisam permanecer separados: o RGBA16 escreve halfwords alternados e sua paridade depende da posição absoluta do pixel.
2. Os testes de overflow em linhas posteriores são especialmente importantes: validar apenas o endereço inicial não garante que toda a região esteja dentro da RDRAM.
3. A seleção de campo pode fazer com que linhas não selecionadas não sejam tocadas; o caso de overflow ignorado para campo não selecionado documenta esse comportamento específico.
4. O inventário descreve o que os testes internos verificam. Para promover qualquer caso a vetor dourado de conformidade, ainda será necessário confrontar o resultado com uma fonte independente e registrar sua procedência.

**Limite desta revisão:** não foi alterado código C++ nem acrescentado um executor JSON. Esta seção é um mapa de cobertura dos testes já existentes e não amplia o suporte funcional do RDP.



### 18.12 Inventário dos testes de comandos de triângulo

Foi revisado o conjunto atual de testes de triângulos em `tests/rdp_tests.cpp`. A cobertura existente é de enquadramento, decodificação e consumo seguro dos comandos; ela **não** valida rasterização nem pixels.

| Caso | Verificação |
|---|---|
| `testTriangleCommandLengths` | Comprimento dos oito opcodes de triângulo, conforme a presença dos blocos opcionais. |
| `testTriangleEdgeHeaderDecoding` | Campos LOD/tile/Y, direção left-major e valores X/inclinações com sinal. |
| `testTriangleCommandPacketDecoding` | Decodificação do pacote completo e preservação dos coeficientes shade, texture e Z. |
| `testTriangleOptionalBlockLayout` | Ordem e offsets dos blocos edge, shade, texture e Z; falha não altera a saída anterior. |
| `testMaximumTriangleCommandWaitsForAllWords` | O fluxo DPC aguarda as 22 palavras do comando máximo antes de consumi-lo. |
| `testTriangleCommandsAreConsumedAsUnsupportedUntilRasterizationExists` | O processador consome o comando completo e reporta suporte ausente, sem fingir que desenhou. |

**O que essa cobertura garante:** os oito formatos de pacote são enquadrados; os campos decodificados são retidos; os blocos opcionais seguem os offsets esperados; e o fluxo não consome um comando incompleto. O estado do processador deixa explícito que o último triângulo foi recebido, mas a execução de desenho ainda não existe.

**O que ela não garante:** interpretação correta do edge walker, regras de inclusão de bordas, cobertura por subpixel, interpolação de atributos, antialiasing, depth compare/update, escrita de framebuffer ou conformidade bit a bit. Um pacote decodificado corretamente pode ainda produzir pixels incorretos quando a rasterização for implementada.

**Próximo passo de validação:** antes de escrever o edge walker, criar fixtures de entrada independentes para triângulos left-major/right-major, YH/YM/YL, inclinações positivas/negativas e casos de uma scanline. Cada fixture precisa ter procedência documentada e resultado esperado independente; testes que apenas repetem a lógica do decoder não podem servir como oráculo.

**Limite desta revisão:** somente documentação foi alterada. Não foi iniciada implementação de rasterização nem mudança no comportamento do processador.


### 18.13 Inventário dos testes de retângulos de textura e carregamento de TMEM

Foi revisada a cobertura existente em `tests/rdp_tests.cpp` para os comandos de retângulo de textura e para as operações de transferência de textura. Assim como nas seções anteriores, este inventário descreve testes internos; não representa, por si só, conformidade independente com o hardware.

**Retângulos de textura (`0x24` e `0x25`):**

| Caso | Verificação |
|---|---|
| `testTextureRectangleLengths` | Os comandos normal e flip possuem dois words. |
| `testTextureRectanglePacketDecoding` | Coordenadas, tile, S/T com sinal, derivadas, flag flip e words brutos são preservados; pacote inválido não altera a saída anterior. |
| `testTextureRectangleCommandsAreConsumedAsUnsupportedUntilRasterizationExists` | O processador consome os dois words, mantém o estado decodificado e retorna `Unsupported`, sem alegar que a textura foi desenhada. |

A cobertura garante enquadramento e retenção dos campos decodificados para as duas variantes. Não verifica amostragem de TMEM, coordenadas de textura, filtragem, perspectiva, ciclo do combiner/blender ou pixels finais. O retorno `Unsupported` é intencional enquanto não existir rasterização.

**Transferência de dados para TMEM:**

- `LOAD_TILE`: os testes cobrem cópia de imagem, stride por linha, coordenadas de origem, coordenadas invertidas, validação de todas as linhas antes da escrita, limites da imagem de origem e ausência de escrita parcial.
- Formatos de 4 bits: há casos para seleção de nibble inicial ímpar, empacotamento de nibbles, linhas com swizzle e wrap no fim da TMEM.
- Formatos de 16 bits: há casos para swizzle de linhas ímpares e wrap no limite da TMEM.
- Formato RGBA32: há casos para divisão entre bancos de TMEM, alinhamento do banco superior, coordenada X diferente de zero, wrap dos dois bancos e validação completa da origem antes de escrever qualquer banco.
- `LOAD_BLOCK`: os testes cobrem cópia contígua, continuidade entre linhas, stride DXT, carries múltiplos, progresso fracionário, troca de words em linhas ímpares, wrap da TMEM e rejeição de origem fora da RDRAM ou da largura da imagem.
- `LOAD_TLUT`: os testes verificam replicação das entradas na região alta da TMEM, coordenadas e intervalo da imagem, além de rejeições por origem ou destino fora dos limites e por destino abaixo da região reservada à paleta.

**Cuidados de interpretação:**

1. Os testes de transferência validam bytes e limites da TMEM, mas não demonstram que o caminho completo de amostragem e filtragem produz pixels corretos.
2. Um teste de swizzle ou wrap bem-sucedido não valida todos os formatos, tamanhos, modos de filtragem ou combinações de tile.
3. Os casos de validação antes da escrita são importantes: uma falha em uma linha posterior não pode deixar a TMEM parcialmente modificada.
4. Os retângulos de textura continuam sendo comandos consumidos sem rasterização. A presença de dados válidos na TMEM não significa que o framebuffer já os utiliza.

**Próxima validação recomendada:** manter separadas três camadas: (a) decodificação e consumo do pacote de retângulo; (b) transferência e organização dos bytes em TMEM; (c) amostragem/rasterização e comparação de pixels. Para iniciar a terceira camada, ainda será necessário um vetor de saída independente, com estado de tile, TMEM, coordenadas, modos e framebuffer esperado documentados.

**Limite desta revisão:** somente documentação foi alterada. Não foram modificados código C++, testes executáveis ou o comportamento do processador RDP.


### 18.14 Inventário dos testes de estado e sincronização do RDP

Foi revisada a cobertura dos comandos de estado em `tests/rdp_tests.cpp`. Esses testes verificam o armazenamento e a decodificação dos registradores que preparam operações posteriores; não demonstram que todos esses estados já são aplicados pelo rasterizador.

| Grupo de comandos | Teste existente | Cobertura observada |
|---|---|---|
| Sincronização | `testRdpSynchronizationCommands` | SYNC_LOAD, SYNC_PIPE, SYNC_TILE e SYNC_FULL atualizam contadores; SYNC_FULL aciona a interrupção DP. |
| Estado básico | `testRdpBasicStateCommands` | SET_SCISSOR, SET_OTHER_MODES, SET_TILE_SIZE, SET_TILE, SET_FILL_COLOR, SET_TEXTURE_IMAGE e SET_COLOR_IMAGE são retidos no estado. |
| Campos de estado | `testRdpStateFieldDecoding` | Decodificação de coordenadas do scissor, campos do tile, tile size e imagens de textura/cor, incluindo formatos e endereços. |
| Modos e combiner | `testRdpOtherModesAndCombineDecoding` | Extração dos campos de ciclo, perspectiva, opções de textura e seletores do combiner. |
| Cores e depth | `testRdpColorAndDepthStateCommands` | SET_FILL_COLOR, SET_FOG_COLOR, SET_BLEND_COLOR, SET_PRIM_COLOR, SET_ENV_COLOR e SET_Z_IMAGE. |
| Conversão e keying | `testRdpConvertAndKeyStateCommands` | Coeficientes SET_CONVERT com extensão de sinal e parâmetros SET_KEY_GB/SET_KEY_R. |
| Profundidade primitiva | `testRdpPrimitiveDepthCommand` | SET_PRIM_DEPTH extrai Z e delta-Z e preserva o comando bruto. |

**Leitura correta do estágio atual:**

1. O processador já reconhece e guarda uma parte ampla dos registradores de configuração do RDP.
2. Decodificar um campo de SET_COMBINE ou SET_OTHER_MODES não significa que o combiner ou os modos de ciclo estejam produzindo efeitos na imagem.
3. O estado de scissor já participa do caminho de FILL_RECTANGLE testado; isso não comprova sua aplicação geral a triângulos e retângulos de textura.
4. A presença de cores primária, ambiente, fog e blend, assim como de Z_IMAGE e PRIM_DEPTH, não comprova a implementação dos estágios de interpolação, comparação de profundidade ou blending.
5. Os testes de sincronização validam contadores e interrupção DP no modelo atual; não substituem uma validação temporal completa do pipeline de hardware.

**Próxima etapa recomendada:** elaborar uma matriz de dependências entre registradores e operações que efetivamente os consomem. Para cada campo, registrar: comando de origem, estrutura de estado, consumidor implementado, teste de efeito observável e lacuna restante. Isso evita confundir suporte de decodificação com suporte funcional e ajuda a escolher o próximo teste de integração sem iniciar prematuramente o rasterizador.

**Limite desta revisão:** somente documentação foi alterada. Não foram modificados código C++, testes executáveis ou o comportamento do processador RDP.


### 18.15 Matriz de consumidores reais do estado RDP

A matriz foi conferida diretamente contra `RdpSyncState` em `app/src/main/cpp/n64/rdp.h` e o `switch` de `RdpCommandProcessor::processNext` e a função `fillRectangle` em `app/src/main/cpp/n64/rdp.cpp`. Ela distingue armazenamento/decodificação de consumo efetivo.

| Estado / comando | O que o processador faz hoje | Consumidor funcional identificado |
|---|---|---|
| SET_SCISSOR | Decodifica coordenadas e flags, guarda comando bruto e estado | `fillRectangle` aplica limites e seleção de campo quando `hasScissor` está ativo. |
| SET_COLOR_IMAGE | Decodifica formato, tamanho, largura e endereço | `fillRectangle` usa formato/tamanho/largura/endereço como destino. |
| SET_FILL_COLOR | Guarda cor de preenchimento e comando bruto | `fillRectangle` usa a cor quando o estado está presente. |
| SET_OTHER_MODES | Decodifica campos e guarda o comando | `fillRectangle` consulta `cycleType` e só aceita o ciclo FILL (valor 3). |
| SET_TEXTURE_IMAGE | Guarda formato, tamanho, largura e endereço da imagem de origem | `LOAD_TILE`, `LOAD_BLOCK` e `LOAD_TLUT` consultam os dados da imagem durante a transferência. |
| SET_TILE | Decodifica e guarda configuração por tile | Rotinas de transferência de textura consultam a configuração do tile selecionado. |
| SET_TILE_SIZE | Decodifica e guarda coordenadas por tile | A configuração é mantida; a amostragem/rasterização que aplicaria o tile size ainda não está integrada. |
| LOAD_TILE / LOAD_BLOCK / LOAD_TLUT | Decodifica intervalo e tile e tenta executar a transferência | Rotinas de cópia validam a imagem/tile e escrevem na TMEM; sucesso é observável nos testes de memória. |
| SET_Z_IMAGE / SET_PRIM_DEPTH | Guarda endereço Z, profundidade primitiva e delta-Z | Nenhum consumidor de comparação/escrita de profundidade está presente no caminho de desenho atual. |
| SET_COMBINE | Decodifica e guarda seletores dos dois ciclos | Nenhum combiner de pixels está integrado. |
| SET_FOG_COLOR / SET_BLEND_COLOR / SET_PRIM_COLOR / SET_ENV_COLOR | Decodifica e guarda cores e campos LOD | Nenhum estágio de fog, blender ou interpolação de cores está integrado. |
| SET_CONVERT / SET_KEY_GB / SET_KEY_R | Decodifica e guarda coeficientes e parâmetros | Nenhum estágio de conversão YUV ou keying está integrado. |
| SET_OTHER_MODES (campos além do ciclo) | Decodifica e guarda flags e seletores | Não há evidência de consumo geral por rasterização; não inferir suporte a AA, Z, filtragem, blender ou ciclos 1/2. |
| SYNC_LOAD / SYNC_PIPE / SYNC_TILE / SYNC_FULL | Atualiza contadores; SYNC_FULL sinaliza interrupção DP | Efeito de controle/interruptor validado por teste; não é um estágio de geração de pixels. |
| Triângulos e retângulos de textura | Decodifica pacote e retém o último comando | Retorna `Unsupported`; não há consumidor rasterizador. |

**Conclusões técnicas:**

- O caminho de escrita de pixels atualmente demonstrado é restrito a FILL_RECTANGLE em ciclo FILL, formatos RGBA16/RGBA32 e estado de destino/cor válido.
- O caminho de transferência de textura movimenta dados para TMEM, mas isso não equivale a amostrar texturas ou renderizar triângulos/retângulos texturizados.
- Vários registradores estão corretamente decodificados e preservados para diagnóstico/futuro uso; não devem ser apresentados como funcionalidades gráficas completas.
- A matriz é uma fotografia do código atual. Ao adicionar consumidores, atualizar a linha correspondente e incluir um teste que observe o efeito, não apenas o valor armazenado.

**Ordem segura sugerida:** (1) manter os testes atuais de FILL_RECTANGLE e transferência TMEM; (2) planejar testes independentes para amostragem de textura isolada; (3) validar edge walker e cobertura com vetores de referência; (4) somente depois conectar interpolação, Z, combiner e blender. Nenhuma implementação de rasterização deve ser iniciada com base apenas nesta matriz.

**Limite desta revisão:** somente documentação foi alterada. Nenhum arquivo C++ ou teste executável foi modificado.


### 18.16 Matriz de limites das transferências para TMEM

Esta revisão cruza as rotinas de transferência em `app/src/main/cpp/n64/rdp.cpp` com os testes registrados em `tests/rdp_tests.cpp`. O foco é identificar o que realmente chega à memória de textura e quais combinações continuam fora do escopo. Transferir bytes para TMEM não significa que esses bytes já possam ser amostrados ou desenhados.

| Operação | Caminho observado no código | Limites e validações observados |
|---|---|---|
| `LOAD_TILE`, 4/8/16 bits | Calcula origem por coordenadas e largura da imagem; destino usa `tile.tmem` e stride `tile.line`; linhas ímpares aplicam XOR 4 no endereço de destino. | Valida o retângulo de origem antes de alterar TMEM; trata nibble inicial/final para 4 bits; wrap de endereço é mascarado para 4 KiB. |
| `LOAD_TILE`, 32 bits | Caminho separado para RGBA32: R/G ficam no banco inferior de 2 KiB e B/A no banco superior; stride continua vindo de `tile.line`. | Exige imagem e tile RGBA de 32 bits; valida a origem inteira antes de escrever qualquer banco; normaliza endereço no banco inferior antes de derivar o superior. |
| `LOAD_BLOCK` | Copia uma sequência contígua de texels de 8 ou 16 bits; acumulador DXT avança a linha e provoca troca das metades de 32 bits em linhas ímpares. | Rejeita tamanhos diferentes de 8/16 bits, contagens excessivas e origem fora da imagem/RDRAM; prepara o buffer temporário antes de escrever TMEM; atualiza tile size após sucesso. |
| `LOAD_TLUT` | Lê entradas de 16 bits e replica cada entrada quatro vezes em um word de 64 bits na região alta da TMEM. | Exige imagem de 16 bits, até 256 entradas e destino de tile na região reservada à paleta; valida origem e intervalo do destino antes da escrita; atualiza tile size após sucesso. |

**Cobertura de regressão identificada:**

- `LOAD_TILE`: cópia básica, stride, wrap, nibble inicial ímpar, empacotamento de 4 bits, swizzle de linhas ímpares em 4/16 bits, separação RGBA32 em bancos, alinhamento e wrap dos bancos, coordenada X não zero e validação de origem sem escrita parcial.
- `LOAD_BLOCK`: cópia contígua, wrap, origem fora da RDRAM, DXT com múltiplos carries e frações, continuidade entre linhas da imagem, início fora da largura e rejeição de tamanhos não suportados.
- `LOAD_TLUT`: replicação de entradas, coordenadas de origem, intervalo além da largura, origem fora dos limites e destino abaixo/acima da região permitida.

**Limites importantes para não superestimar o suporte:**

1. O caminho ordinário de `LOAD_BLOCK` aceita apenas texels de 8 e 16 bits; 4 bits e 32 bits são explicitamente rejeitados nesse caminho.
2. O caminho de 32 bits de `LOAD_TILE` é restrito a RGBA32. Não se deve generalizar esse suporte para outros formatos de 32 bits.
3. As rotinas demonstram organização e transferência de dados, inclusive swizzle, wrap e DXT nos casos implementados. Ainda não demonstram leitura desses dados pelo sampler, aplicação de `SET_TILE_SIZE`, clamp/mirror/mask/shift, palette lookup, filtro bilinear ou perspectiva.
4. Os testes de validação prévia protegem contra escrita parcial em várias falhas de origem. Essa propriedade deve continuar coberta quando novos formatos forem adicionados.
5. A matriz é derivada do código e dos testes internos; ainda não é um conjunto de resultados dourados comparados independentemente com hardware ou outro emulador.

**Próximo passo seguro:** antes de implementar amostragem, definir fixtures isoladas que especifiquem bytes iniciais da TMEM, configuração completa do tile, coordenadas S/T e resultado esperado para formatos CI/IA/I/RGBA, incluindo limites de tile, wrap, clamp/mirror e TLUT. Cada resultado esperado precisa ter procedência independente. Até isso existir, manter o escopo em documentação e testes das transferências já implementadas.

**Limite desta revisão:** somente documentação foi alterada. Nenhum código C++ ou teste executável foi modificado; nenhuma funcionalidade de amostragem ou rasterização foi declarada como implementada.


### 18.17 Requisitos do sampler de textura e ordem de validação

Foi feita uma leitura cruzada do capítulo 13 do *Nintendo 64 Programming Manual* e do shader de referência `parallel-rdp/shaders/texture.h` (projeto público de emulação). O objetivo é delimitar o próximo subsistema sem copiar código nem assumir que a implementação de referência é, por si só, uma especificação normativa.

**Fluxo conceitual confirmado pelo manual:**

1. O comando `SET_TILE` configura formato, tamanho, stride em words de 64 bits, base TMEM, paleta e parâmetros por eixo (clamp, mirror, mask e shift).
2. `SET_TILE_SIZE` fornece os limites SL/TL/SH/TH usados durante a renderização.
3. Os comandos `LOAD_TILE`, `LOAD_BLOCK` e `LOAD_TLUT` alimentam TMEM; os comandos de carga também alteram os limites do tile, e o manual recomenda reaplicar `SET_TILE_SIZE` depois de `LOAD_BLOCK`/`LOAD_TLUT` quando necessário.
4. Durante a renderização, as coordenadas S/T são transformadas em coordenadas relativas ao tile, com shift, clamp, wrap e mirror; só então são usadas para localizar texels em TMEM.
5. A etapa de busca precisa decodificar o formato/tamanho, tratar TLUT quando habilitada e selecionar amostragem pontual ou filtrada. O manual também descreve footprints de 2×2 texels e filtragem/interpolação em modos de ciclo apropriados.

**Comparação com o código atual de M64plus:**

| Recurso | Estado atual identificado | Trabalho ainda necessário |
|---|---|---|
| Descritor de tile | Formato, tamanho, line, TMEM, palette, clamp/mirror/mask/shift são armazenados em `RdpTileState`. | Aplicar os campos na transformação de S/T durante uma operação de desenho. |
| Limites do tile | `RdpTileSizeState` é armazenado; `LOAD_BLOCK` e `LOAD_TLUT` também atualizam os limites após sucesso. | Usar SL/TL/SH/TH na normalização/clamp do sampler e validar a interação com `SET_TILE_SIZE`. |
| Busca em TMEM | Existem rotinas de escrita/carga e acesso por byte/word. | Decodificar texels por formato/tamanho, incluindo alinhamento, bancos e swizzle. |
| TLUT | `LOAD_TLUT` replica entradas de 16 bits na região alta da TMEM. | Resolver índices CI4/CI8, seleção de paleta e conversão RGBA16/IA16. |
| Coordenadas | Triângulos retêm coeficientes STW; retângulos de textura retêm S/T e derivadas. | Interpolação/perspective divide e transformação por tile antes da busca. |
| Filtragem | Flags de sample type, mid-texel e bilerp são decodificadas em other modes. | Implementar ponto, filtro RDP e vizinhos com wrap/mirror/clamp corretos. |
| Ciclos | Campos de ciclo e combiner são armazenados. | Integrar Texel0/Texel1 e a ordem de amostragem ao pipeline de um/dois ciclos. |

**Observações específicas do shader de referência:** a implementação pública separa transformação de coordenadas, máscara/mirror, busca por formato, TLUT e filtragem. O código mostra que a ordem importa: shift e ajuste pelo limite inferior antecedem a seleção de texel; a máscara/mirror determina também o texel vizinho usado na filtragem; e o filtro de três pontos não é simplesmente uma média bilinear convencional. O shader inclui caminhos distintos para CI com TLUT, YUV, RGBA/IA/I e diferentes tamanhos. Esses detalhes são bons alvos de estudo e comparação, mas cada comportamento deve ser confirmado por documentação ou vetor de referência antes de ser adotado.

**Plano de testes independente, antes de integrar ao rasterizador:**

- Camada A — transformação de coordenadas: shift S/T (incluindo os códigos de shift à esquerda), subtração de SL/TL, clamp superior/inferior, wrap por mask e mirror em cada eixo.
- Camada B — leitura de texel: RGBA16/32, IA4/8/16, I4/8, CI4/8 sem TLUT e YUV, com endereços TMEM conhecidos e testes de swizzle/bancos.
- Camada C — paleta: CI4 com cada palette base, CI8, TLUT RGBA16 e IA16, entradas nos limites e seleção de tipo.
- Camada D — filtro: point sample, footprint 2×2, filtro de três pontos, mid-texel e vizinhos cruzando borda de wrap/mirror/clamp.
- Camada E — integração: apenas depois das camadas anteriores, conectar S/T interpolados de retângulo e triângulo e comparar framebuffer completo.

Cada fixture deve guardar estado do tile, bytes da TMEM, coordenadas fixas, modos relevantes e resultado esperado com referência/procedência. Um teste que calcula o resultado esperado usando a mesma função que está sendo testada não é um oráculo independente.

**Fontes consultadas:**

- Nintendo 64 Programming Manual, capítulo 13 — Texture Mapping, especialmente [13.1 Texture Mapping](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-01.html), [13.4 Tile Attributes](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-04.html), [13.5 Tile Descriptor Loading](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-05.html) e [13.7 Texture Filtering](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-07.html).
- Themaister/parallel-rdp, [shader de textura](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/texture.h). Consultado como implementação independente para estudar a separação de etapas e casos de borda, não como substituto do manual ou de vetores dourados.

**Limite desta revisão:** somente documentação foi alterada. Nenhum código C++ ou teste executável foi modificado. O sampler continua não implementado; esta seção não declara suporte de renderização de textura.


### 18.18 Inventário de fixtures para iniciar um sampler isolado

A revisão dos testes executáveis em `tests/rdp_tests.cpp` confirma uma base consistente para transferências, mas não encontrou testes de leitura/amostragem de texels. Os testes existentes verificam efeitos observáveis em TMEM para `LOAD_TILE`, `LOAD_BLOCK` e `LOAD_TLUT`; isso não valida a interpretação posterior desses bytes pelo sampler.

**Cobertura já existente que pode servir de preparação:**

- `LOAD_TILE`: cópia de 4/8/16 bits, nibble inicial ímpar, stride, swizzle de linhas ímpares, wrap de TMEM, RGBA32 em bancos separados e validação prévia de todas as linhas.
- `LOAD_BLOCK`: cópia de 8/16 bits, DXT com carries e frações, mudança de linha, wrap e rejeição de tamanhos não suportados.
- `LOAD_TLUT`: replicação de entradas de 16 bits na região alta da TMEM, coordenadas não nulas e rejeição de origem/destino fora dos limites.
- A função `main()` registra explicitamente os testes. Qualquer novo teste C++ precisa também ser chamado ali, pois o build trata avisos como erros.

**Fixtures mínimas propostas para o primeiro módulo independente (sem rasterizador):**

| Fixture | Estado e dados controlados | Resultado que deve ser fixado por oráculo externo |
|---|---|---|
| RGBA16 direto | TMEM com dois padrões distintos, tile RGBA/16, coordenada S/T inteira | Conversão exata do texel para os canais de saída, incluindo expansão dos bits e alpha. |
| RGBA32 direto | Bytes conhecidos nos bancos inferior/superior, tile RGBA/32 | Reconstrução exata dos quatro canais e endereço físico consultado. |
| I4/I8 e IA4/IA8/IA16 | Padrões com valores mínimos, máximos e intermediários | Decodificação de intensidade/alpha, ordem de nibbles e escala de cada componente. |
| CI4/CI8 + TLUT | Índices repetidos e distintos; paleta RGBA16 e IA16 | Índice selecionado, entrada da paleta e conversão final, inclusive limites de palette CI4. |
| Coordenadas por eixo | Mesma TMEM e texel de cores únicas em cada posição | Resultado para shift, subtração do limite inferior, clamp, mask, mirror e wrap, testados separadamente. |
| Borda e filtro | Textura 2×2 com quatro cores diferentes | Texel escolhido no point sample e contribuição de cada vizinho nos modos filtrados; casos de borda precisam atravessar wrap/mirror/clamp. |

**Regras para que os testes sejam realmente independentes:**

1. Separar a transformação S/T, o cálculo de endereço TMEM, a decodificação do texel e a filtragem em funções testáveis; não misturar interpolação de triângulos neste primeiro estágio.
2. Cada fixture deve declarar explicitamente formato, tamanho, line, TMEM base, palette, SL/TL/SH/TH, parâmetros S/T e modos relevantes. O padrão da TMEM deve ser pequeno e legível manualmente.
3. Guardar o resultado esperado e a fonte que o fundamenta. Não derivar o esperado da mesma rotina sob teste, nem copiar uma saída gerada pelo próprio emulador.
4. Começar com amostragem pontual e coordenadas inteiras. Adicionar coordenadas fracionárias e filtro somente após os endereços e a decodificação básica passarem.
5. Manter casos negativos para tile/formato inválido e acessos que excedam os limites físicos de TMEM; falhas não devem produzir leituras fora do buffer.
6. Registrar cada função de teste em `main()` e executar ambos os jobs de CI antes de considerar a camada validada.

**Lacunas ainda não resolvidas:** os testes atuais não constituem oráculo de amostragem; a semântica completa de filtragem RDP, arredondamento, precisão intermediária e casos de borda ainda precisa ser respaldada por documentação técnica e vetores independentes. O shader de `parallel-rdp` continua sendo referência de comparação, não prova normativa isolada.

**Próxima etapa segura:** construir primeiro uma especificação curta e revisável para RGBA16/32 com point sample e coordenadas inteiras, anexando vetores dourados independentes. Só depois dessa especificação e de autorização explícita deve haver alteração de C++.

**Limite desta revisão:** somente documentação foi alterada. Nenhum código C++, teste executável ou comportamento do emulador foi modificado.


### 18.19 Primeiro vetor dourado proposto: conversão RGBA16/RGBA32

A tabela de formatação de texels do *Nintendo 64 Programming Manual* define RGBA16 como R5/G5/B5/A1 e RGBA32 como R8/G8/B8/A8. Para RGBA16, os cinco bits de cada canal são expandidos para oito pela repetição dos bits mais significativos; o alpha de um bit vira 0 ou 255. Para RGBA32, os quatro bytes já correspondem diretamente aos canais RGBA. Este vetor valida somente a decodificação de um texel, não coordenadas, TMEM, filtragem ou rasterização.

**Vetor RGBA16 (palavras de 16 bits):**

| Palavra | R5/G5/B5/A1 | RGBA8 esperado |
|---|---|---|
| `0xF801` | 31 / 0 / 0 / 1 | `FF 00 00 FF` |
| `0x07C1` | 0 / 31 / 0 / 1 | `00 FF 00 FF` |
| `0x003F` | 0 / 0 / 31 / 1 | `00 00 FF FF` |
| `0x0000` | 0 / 0 / 0 / 0 | `00 00 00 00` |
| `0xFFFF` | 31 / 31 / 31 / 1 | `FF FF FF FF` |

A expansão dos canais de cinco bits é `(v << 3) | (v >> 2)`. O teste deve comparar cada canal separadamente e incluir valores intermediários, por exemplo 1, 15 e 16, para detectar expansão incorreta por simples deslocamento.

**Vetor RGBA32:**

| Bytes do texel em ordem RGBA | RGBA8 esperado |
|---|---|
| `12 34 56 78` | `12 34 56 78` |
| `00 80 FF 01` | `00 80 FF 01` |
| `FF 00 7F 00` | `FF 00 7F 00` |

**Escopo e pré-condições:** os vetores acima são casos de decodificação isolada, com a palavra RGBA16 já interpretada como valor de 16 bits e os bytes RGBA32 apresentados na ordem lógica dos canais. Um teste futuro de integração com TMEM ainda precisará fixar a ordem de bytes, endereçamento, bancos de 32 bits e stride conforme o caminho de carga correspondente. Não assumir que o resultado deste teste valida sozinho a disposição física da TMEM.

**Separação recomendada dos testes:** (a) função pura de expansão/decodificação RGBA16 e RGBA32; (b) leitor de texel que calcula endereço a partir de tile/TMEM; (c) transformação de coordenadas; (d) integração com o sampler. A etapa (a) pode ser testada sem implementar as demais, mas sua adição ao C++ depende de autorização explícita para alterar código.

**Fonte normativa:** Nintendo 64 Programming Manual, seção Texture Mapping, tabela “Texel Output Formatting” (no PDF completo, capítulo 14, tabela 14-9), que especifica os campos de entrada e saída para I, IA, RGBA e demais formatos: https://ultra64.ca/files/documentation/nintendo/Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf. A seção 13.4 também documenta os códigos de formato/tamanho dos descritores de tile: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-04.html.

**Limite desta revisão:** somente documentação foi alterada. Nenhum código C++ ou teste executável foi modificado; os vetores são uma proposta documentada, ainda não executada pelo projeto.

### 18.20 Vetores dourados propostos: decodificação I4/I8 e IA4/IA8/IA16

Esta seção amplia a etapa de decodificação isolada iniciada em 18.19. O escopo é transformar um valor lógico de texel já extraído em intensidade/alpha de 8 bits. Não valida endereço TMEM, ordem física de bytes/nibbles, TLUT, coordenadas, filtragem ou rasterização.

**Regras de formato a validar:**

| Formato | Campos lógicos | Conversão para 8 bits |
|---|---|---|
| I4 | I[3:0] | Replicar o nibble: `(I << 4) | I`. |
| I8 | I[7:0] | Valor direto. |
| IA4 | I[3:1], A[0] | Expandir I3 para 8 bits; A1 vira 0 ou 255. |
| IA8 | I[7:4], A[3:0] | Expandir cada nibble: `(v << 4) | v`. |
| IA16 | I[15:8], A[7:0] | Os dois bytes são intensidade e alpha, sem expansão. |

Para IA4, a expansão de intensidade de três bits proposta é `(v << 5) | (v << 2) | (v >> 1)`. Isso replica os bits disponíveis para preencher oito bits: 0→0, 1→36, 2→73, 3→109, 4→146, 5→182, 6→219 e 7→255. O bit de alpha é independente da intensidade.

**Vetores I:**

| Formato e valor lógico | Intensidade esperada |
|---|---|
| I4 `0x0` | `0x00` |
| I4 `0x1` | `0x11` |
| I4 `0x7` | `0x77` |
| I4 `0x8` | `0x88` |
| I4 `0xF` | `0xFF` |
| I8 `0x00` | `0x00` |
| I8 `0x01` | `0x01` |
| I8 `0x7F` | `0x7F` |
| I8 `0x80` | `0x80` |
| I8 `0xFF` | `0xFF` |

**Vetores IA:**

| Formato e valor lógico | I esperado | A esperado |
|---|---:|---:|
| IA4 `0b0000` | `0x00` | `0x00` |
| IA4 `0b0011` | `0x24` | `0xFF` |
| IA4 `0b0110` | `0x49` | `0x00` |
| IA4 `0b1001` | `0x92` | `0xFF` |
| IA4 `0b1111` | `0xFF` | `0xFF` |
| IA8 `0x00` | `0x00` | `0x00` |
| IA8 `0x1F` | `0x11` | `0xFF` |
| IA8 `0x80` | `0x88` | `0x00` |
| IA8 `0xAF` | `0xAA` | `0xFF` |
| IA8 `0xFF` | `0xFF` | `0xFF` |
| IA16 `0x0000` | `0x00` | `0x00` |
| IA16 `0x1280` | `0x12` | `0x80` |
| IA16 `0x7FFF` | `0x7F` | `0xFF` |
| IA16 `0xFF00` | `0xFF` | `0x00` |
| IA16 `0xFFFF` | `0xFF` | `0xFF` |

**Cuidados para a implementação futura:**

1. Os valores acima são campos lógicos já extraídos. Em I4/IA4, o teste de leitura da TMEM deverá especificar separadamente qual nibble corresponde ao texel par/ímpar; não inferir essa ordem a partir desta tabela.
2. Em IA4, os três bits mais significativos são intensidade e o bit menos significativo é alpha. Em IA8, o nibble alto é intensidade e o baixo é alpha. Em IA16, o byte alto é intensidade e o baixo é alpha.
3. Não aplicar a expansão de nibble de IA8/I4 a I8/IA16: esses formatos já carregam canais de oito bits.
4. O resultado de IA é um par intensidade/alpha. A conversão posterior para RGBA (por exemplo, replicar I nos canais RGB) deve ser testada em uma camada separada, para não misturar decodificação do formato com convenções do consumidor.
5. Cada teste deve comparar canais individualmente e incluir mínimos, máximos e valores intermediários. Os vetores não substituem testes de endereçamento, alinhamento, TMEM banks ou swizzle.

**Fonte normativa consultada:** Nintendo 64 Programming Manual, capítulo 14, tabela 14-9 “Texel Output Formatting”, no [PDF completo](https://ultra64.ca/files/documentation/nintendo/Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf). Os vetores são derivados dos campos de bits descritos nessa tabela e das expansões de bits indicadas; ainda precisam ser implementados e executados no projeto.

**Limite desta revisão:** somente documentação foi alterada. Nenhum código C++ ou teste executável foi modificado. Esta seção não declara que I/IA ou o sampler estejam implementados.

### 18.21 Registro de validação de CI das especificações de texel

Em 25/11/2026, foram verificadas as execuções de CI associadas aos três commits que documentaram os primeiros vetores de decodificação:

| Conteúdo | Commit | Execução do GitHub Actions | Resultado |
|---|---|---|---|
| Vetores iniciais RGBA16/RGBA32 (18.19) | `2bbd7908195cbc33ded9e07dbc7a80c4ace7b2fd` | [45580827796](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45580827796) | Sucesso |
| Vetores I4/I8/IA4/IA8/IA16 (18.20) | `2534b16ae3ac299cbfb4fb723ff9c68c34c99259` | [45581263039](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45581263039) | Sucesso |
| Correção do vetor IA4 (intensidade `0x92`) | `3c350d2fc60d78534aac7a4fa196f3b20ef61d1c` | [45581282239](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45581282239) | Sucesso |

**O que esse resultado comprova:** os workflows configurados para o repositório terminaram com sucesso nesses commits. A execução inclui o job de testes nativos e a montagem Android Debug.

**O que não comprova:** os vetores das seções 18.19 e 18.20 ainda são especificações documentadas, não testes executáveis do decodificador. O CI verde não significa que RGBA/I/IA já sejam lidos pelo sampler, nem que exista renderização de triângulos ou texturas.

**Próximo passo técnico:** transformar os vetores de conversão isolada em testes C++ somente após autorização explícita para modificar código. Depois, adicionar testes separados para leitura de TMEM, mantendo a ordem: extração do texel, decodificação do formato e integração com o sampler.

**Limite desta revisão:** esta seção registra evidência do CI e não altera a avaliação de funcionalidades implementadas. Nenhum código C++ ou teste executável foi modificado.



### 18.22 Especificação de CI4/CI8 e TLUT antes do sampler

Esta etapa consolida o comportamento documentado de texturas color-indexed (CI) sem declarar que a busca de paleta já esteja implementada. O objetivo é separar três operações que não devem ser confundidas: extrair o índice do texel da metade baixa da TMEM, formar o índice da paleta e decodificar a entrada de 16 bits da TLUT.

**Organização física e regras do índice:**

| Caso | Dados da textura | Formação do índice da TLUT | Limite lógico |
|---|---|---|---|
| CI4 | Texel de 4 bits na metade baixa da TMEM | `(palette << 4) | texel` | 16 paletas × 16 entradas |
| CI8 | Texel de 8 bits na metade baixa da TMEM | O próprio texel de 8 bits | 256 entradas |

A metade alta da TMEM é reservada à TLUT durante o uso de CI. CI4 usa o campo `palette` do descritor de tile como os quatro bits superiores do índice; o texel fornece os quatro bits inferiores. CI8 usa diretamente o índice de oito bits. A TLUT armazena entradas de 16 bits, interpretadas conforme o modo global selecionado: RGBA16 ou IA16.

**Bancos e replicação:** o RDP pode consultar quatro texels em paralelo. Por isso, a organização da TLUT replica cada entrada nos quatro bancos correspondentes. O manual descreve quatro cópias da tabela na metade alta da TMEM; o código atual de `LOAD_TLUT` também grava cada entrada de 16 bits repetida em uma palavra de 64 bits. Isso é uma correspondência estrutural importante, mas ainda não prova que o endereço consultado pelo futuro sampler esteja correto para todos os casos.

**Vetores lógicos propostos para CI4:**

| Palette | Texel | Índice esperado | Entrada TLUT a selecionar |
|---:|---:|---:|---|
| 0 | 0 | `0x00` | 0 |
| 0 | 15 | `0x0F` | 15 |
| 1 | 0 | `0x10` | 16 |
| 1 | 5 | `0x15` | 21 |
| 7 | 9 | `0x79` | 121 |
| 15 | 15 | `0xFF` | 255 |

**Vetores lógicos propostos para CI8:**

| Texel CI8 | Índice esperado | Entrada TLUT a selecionar |
|---:|---:|---:|
| `0x00` | `0x00` | 0 |
| `0x01` | `0x01` | 1 |
| `0x7F` | `0x7F` | 127 |
| `0x80` | `0x80` | 128 |
| `0xFE` | `0xFE` | 254 |
| `0xFF` | `0xFF` | 255 |

Esses vetores fixam somente a formação do índice lógico. Não especificam o endereço em bytes da TMEM, o mapeamento de bancos, a ordem física do texel CI4 par/ímpar nem a codificação física da entrada replicada. Esses pontos precisam de fixtures de integração independentes, com padrões distintos por banco para detectar leituras na cópia errada.

**Matriz mínima de fixtures de integração futura:**

1. CI4 com palette 0 e palette 15, usando o mesmo texel para provar que o campo palette altera somente os quatro bits superiores.
2. CI4 com texel 0 e 15 em cada palette, cobrindo as extremidades de cada tabela de 16 entradas.
3. CI8 com índices 0, 127, 128 e 255, cobrindo a transição do bit mais significativo e os limites da TLUT.
4. Cada entrada de paleta deve ter uma cor diferente; repetir a mesma cor em todas as entradas esconderia erros de índice.
5. Executar os mesmos índices com TLUT RGBA16 e IA16, comparando os canais após a decodificação apropriada.
6. Fazer fixtures distintas para leitura dos quatro bancos e para texels CI4 em nibbles alto/baixo; não deduzir a ordem do nibble a partir dos vetores lógicos desta seção.
7. Incluir casos negativos para TLUT ausente/desabilitada e para endereços fora dos limites físicos, sem inventar uma resposta de hardware onde a documentação não a define.

**Observação sobre a implementação atual:** o repositório já possui rotina de carga `LOAD_TLUT` e testes de replicação/limites descritos na seção 18.16. Isso valida a transferência documentada coberta por esses testes; não valida ainda a seleção de índice CI4/CI8 nem a conversão final pelo sampler.

**Fontes consultadas:**
- Nintendo 64 Programming Manual, seção 13.4, atributos de tile e formação do índice CI4: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-04.html.
- Nintendo 64 Programming Manual, capítulo 12, organização em duas metades da TMEM, quatro bancos e replicação da TLUT: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-04.html.
- Nintendo 64 Programming Manual, capítulo 13.8, organização de memória de textura e TLUT: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-08.html.
- Referência das macros `gDPLoadTLUT_pal16` e `gDPLoadTLUT_pal256`, que descreve a localização da TLUT na metade alta: https://ultra64.ca/files/documentation/online-manuals/man/n64man/gdp/gDPLoadTLUT_pal.html.

**Próxima etapa segura:** especificar o endereço físico e a leitura de um texel CI4/CI8 a partir da TMEM, com fixtures que diferenciem bancos e nibbles. A filtragem continua fora de escopo. Nenhuma alteração C++ ou teste executável deve ser feita sem autorização explícita.

**Limite desta revisão:** somente documentação foi alterada. Nenhum código C++, teste executável ou comportamento do emulador foi modificado.


### 18.23 Endereçamento físico de CI4/CI8 na TMEM: limites e fixtures

Esta seção aproxima os índices lógicos da seção 18.22 da organização física da TMEM. O RDP possui 4 KiB de TMEM, acessada em quatro bancos paralelos; para CI, os texels ficam na metade baixa (2 KiB) e a TLUT na metade alta (2 KiB). As entradas da TLUT são replicadas para permitir as quatro consultas paralelas. A documentação também descreve a reorganização de linhas ímpares para texturas de 4, 8 e 16 bits: em cada bloco de oito bytes, os dois grupos de quatro bytes trocam de posição. Fontes: capítulos 12.4 e 13.8 do manual. (Links ao final.)

**Unidades de endereço:**

- O campo `tile.tmem` é contado em palavras de 64 bits; a base em bytes é `tile.tmem * 8`.
- O campo `tile.line` também representa palavras de 64 bits por linha; o stride em bytes é `tile.line * 8`.
- Para CI4, cada byte contém dois índices de 4 bits. A extração lógica precisa distinguir texel par e ímpar; a convenção de nibble usada pelo caminho atual de carga é documentada abaixo, mas ainda precisa de um vetor independente de leitura.
- Para CI8, cada texel é um byte e o valor inteiro seleciona a entrada da TLUT.
- A área de texels CI não pode ocupar a metade alta reservada à TLUT durante o modo CI. O descritor pode ter endereços em toda a TMEM para outros formatos, mas os testes CI devem rejeitar ou classificar separadamente combinações que invadam a região da TLUT.

**Comportamento observado no código de carga atual (não é especificação do sampler):**

1. `loadTileTexels` calcula a base de destino como `tile.tmem * 8` e o stride como `tile.line * 8`.
2. Para fonte de 4 bits, o caminho atual extrai o texel par do nibble alto e o ímpar do nibble baixo. No destino, mantém a mesma convenção de nibble por coluna, combinando os dois texels em um byte.
3. Para linhas ímpares, o caminho de carga aplica XOR 4 ao endereço de byte antes de escrever, trocando os grupos de quatro bytes dentro do bloco de oito bytes.
4. `loadTlutEntries` exige imagem de origem de 16 bits, restringe a carga a até 256 entradas e ao intervalo de palavras TMEM 256–511. Cada entrada de 16 bits é replicada quatro vezes em uma palavra de 64 bits.
5. O fato de a rotina de carga gravar esses bytes não prova que o futuro sampler consulte o mesmo endereço/banco/nibble corretamente. Carga e amostragem devem ter testes separados.

**Fixtures de leitura que devem ser acrescentadas quando houver autorização para código:**

| Fixture | Padrão TMEM | O que deve detectar |
|---|---|---|
| CI4, dois texels no mesmo byte | nibble alto = `0x3`, baixo = `0xC` | Ordem par/ímpar sem confundir os índices |
| CI4, duas palettes | mesmos nibbles, palette 0 e 1 | O palette altera os quatro bits superiores do índice, não o texel |
| CI8, índices extremos | bytes `00`, `7F`, `80`, `FF` | Índice direto e transição do bit 7 |
| Linha par/ímpar | bytes distintos nos offsets 0–7 de duas linhas | Troca dos grupos de quatro bytes apenas na linha ímpar |
| Stride maior que largura | `line` com espaço de preenchimento entre linhas | O endereço da segunda linha usa `line * 8`, não a largura visível |
| TLUT replicada | quatro cópias com a mesma entrada e padrões vizinhos diferentes | A leitura de cada banco alcança a entrada replicada correta |
| TLUT com padrões únicos | entrada i contém valor diferente de i+1 | Índice errado, deslocamento de base e erro de limite |
| Limites CI | tile na palavra 255, tile na palavra 256 e carga de TLUT nos extremos 256/511 | Separação entre região de texel e região de paleta; evitar wrap que esconda overflow |

**Oráculo e critérios de aprovação:**

- A fixture deve inicializar TMEM diretamente com bytes explícitos e registrar o endereço esperado antes de executar a função de leitura.
- Para CI4, comparar separadamente: endereço do byte, seleção do nibble, palette, índice de oito bits, endereço lógico da TLUT e valor RGBA final.
- Para CI8, comparar separadamente: endereço do byte do texel, índice de oito bits, endereço lógico da TLUT e valor final.
- Os quatro bancos devem usar padrões distinguíveis durante o teste; só depois repetir com as quatro cópias idênticas usadas normalmente.
- Não inferir o endereço físico exato de cada banco apenas pela replicação feita por `LOAD_TLUT`. A fórmula de seleção de banco e o swizzle completo precisam ser confrontados com os diagramas de TMEM do manual e vetores independentes.
- Manter a transformação S/T e o filtro fora destes testes. O primeiro leitor deve operar com coordenadas inteiras e sem filtragem.

**Fontes técnicas:**
- Nintendo 64 Programming Manual, capítulo 12.4, quatro bancos acessíveis em paralelo e partição CI/TLUT: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-04.html.
- Nintendo 64 Programming Manual, capítulo 13.8, organização física e lógica da TMEM, layouts de 4/8/16/32 bits e TLUT: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-08.html.
- Nintendo 64 Programming Manual, seção 13.4, `line`, base TMEM em palavras de 64 bits e palette CI4: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-04.html.
- Moria, “Nintendo 64 Part 20: TMEM Format and Mip Maps”, explicação complementar do swizzle de linhas ímpares: https://www.moria.us/blog/2020/11/n64-part20-tmem-format-and-mip-maps. Usar como análise técnica secundária, não como substituto do manual.

**Próxima etapa:** montar uma matriz de endereços para CI4/CI8 que relacione (S,T), `line`, `tmem`, nibble e banco, usando diagramas e casos independentes. A implementação do sampler continua bloqueada até essa matriz e seus vetores serem revisados. Nenhum código C++ ou teste executável foi alterado nesta revisão.


### 18.24 Matriz de endereçamento CI4/CI8 para coordenadas inteiras

Esta matriz fixa uma primeira referência de leitura de texel, antes de shift, clamp, mask, mirror, wrap ou filtragem. As coordenadas S/T desta seção já são inteiras e não estão em formato 10.2. O endereço abaixo é o byte lógico na TMEM; a seleção do banco físico e a consulta paralela dos quatro texels são uma etapa distinta.

**Fórmulas de referência:**

Defina `base = tile.tmem + tile.line * T`, em palavras de 64 bits, e `rowByteBase = base * 8`.

| Formato | Byte do texel antes do swizzle | Nibble |
|---|---|---|
| CI4 | `rowByteBase + floor(S / 2)` | S par: alto; S ímpar: baixo |
| CI8 | `rowByteBase + S` | Não se aplica |

Para linha T ímpar, a organização de bancos troca os dois grupos de quatro bytes de cada palavra de 64 bits. Na representação de bytes canônica usada pelo projeto, isso corresponde a aplicar XOR 4 ao endereço de byte. O endereço de leitura de CI4/CI8 fica, portanto, `byteAddress = (rowByteBase + horizontalOffset) XOR (T ímpar ? 4 : 0)`. O endereço de texel CI deve permanecer na metade baixa da TMEM (0x000–0x7FF); a metade alta é a região da TLUT.

As mesmas fórmulas podem ser expressas diretamente a partir do campo em palavras: CI4 `((base << 4) + S) >> 1`; CI8 `(base << 3) + S`. O endereço final de byte é ajustado pelo swizzle de linha ímpar. A fonte de implementação secundária Angrylion documenta essas expressões em seu caminho de fetch; o manual da Nintendo fundamenta a organização em bancos e a partição CI/TLUT. Não copiar constantes de XOR específicas de host: Angrylion pode compilar com byte-swaps adicionais por armazenar TMEM em palavras nativas little-endian. O projeto usa uma matriz de bytes canônica e deve validar XOR 4 contra seus próprios vetores.

**Exemplo numérico fixo:** `tile.tmem = 0x10`, `tile.line = 2`. Logo, T=0 tem base de linha em `0x80`; T=1 tem base nominal `0x90`, com swizzle XOR 4; T=2 tem base `0xA0`.

| T | S | CI4 byte | CI4 nibble | CI8 byte |
|---:|---:|---:|---|---:|
| 0 | 0 | `0x080` | alto | `0x080` |
| 0 | 1 | `0x080` | baixo | `0x081` |
| 0 | 2 | `0x081` | alto | `0x082` |
| 0 | 3 | `0x081` | baixo | `0x083` |
| 0 | 7 | `0x083` | baixo | `0x087` |
| 1 | 0 | `0x094` | alto | `0x094` |
| 1 | 1 | `0x094` | baixo | `0x095` |
| 1 | 2 | `0x095` | alto | `0x096` |
| 1 | 3 | `0x095` | baixo | `0x097` |
| 1 | 7 | `0x097` | baixo | `0x093` |
| 2 | 0 | `0x0A0` | alto | `0x0A0` |
| 2 | 1 | `0x0A0` | baixo | `0x0A1` |
| 2 | 7 | `0x0A3` | baixo | `0x0A7` |

**Padrão de bytes para a fixture:** inicializar os bytes 0x080–0x087 com `0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0`. Em CI4, a linha T=0 deve produzir a sequência de índices `1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0` ao percorrer os nibbles alto/baixo. Em CI8, os oito primeiros texels são `0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF0`. Para T=1, usar bytes distintos nos offsets 0x090–0x09F e confirmar que o endereço lógico 0x090 é lido fisicamente em 0x094 após o swizzle, sem alterar a seleção de nibble pelo valor de S.

**Paleta:** depois da extração do índice, CI4 forma `index = (tile.palette << 4) | nibble`; CI8 usa `index = texelByte`. Só então ocorre a consulta TLUT. O teste deve comparar índice, entrada selecionada e RGBA resultante em etapas separadas. A tabela de endereço lógico não tenta substituir o mapeamento de quatro bancos da TLUT.

**Divergência importante a resolver antes do sampler:** o código atual de `loadTileTexels` aceita `image.size == 4 bits` e copia nibbles diretamente, enquanto uma referência técnica secundária recente descreve que o fluxo normal de hardware para texturas de 4 bits pode carregar os bytes empacotados usando imagem/LOAD_TILE de 8 bits e interpretar os nibbles no tile de 4 bits. Isso sugere que aceitar diretamente LOAD_TILE com imagem 4-bit pode estar incorreto ou, no mínimo, precisa de prova documental. Não mudar a rotina nem os testes existentes nesta etapa; abrir uma investigação separada sobre as regras de LOAD_TILE/LOAD_BLOCK para 4-bit, usando manual e vetores de hardware/oráculo.

**Fontes:**
- Nintendo 64 Programming Manual, capítulo 12.4, bancos paralelos e partição de TMEM para CI/TLUT: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-04.html.
- Nintendo 64 Programming Manual, capítulo 13.8, diagramas de organização física da TMEM: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-08.html.
- Angrylion RDP Comments, seção “Texture Fetching”, fórmulas de fetch CI4/CI8 e palette; fonte secundária de análise de implementação, não especificação normativa: https://emudev.org/2021/09/21/Angrylion_RDP_Comments.
- RustyN64 RDP progress notes, observação sobre a carga de texturas 4-bit; pista secundária que precisa ser confrontada com documentação/hardware: https://github.com/doublegate/RustyN64/blob/main/docs/rdp.md.

**Critério de saída desta etapa:** antes de escrever o sampler, ter testes independentes para CI4 par/ímpar, CI8, linha par/ímpar, stride maior que a largura, limite 0x7FF, palette 0/15 e leitura das quatro cópias TLUT. A transformação S/T e filtragem permanecem fora do escopo. Nenhum código C++ ou teste executável foi alterado.
