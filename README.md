# M64plus

Emulador Nintendo 64 desenvolvido em C++17 para Android. Alvo inicial: Android 16 / ARM64, com testes planejados no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual do código

- **Android / vídeo de diagnóstico:** aplicativo Android com OpenGL ES 2.0; o triângulo de diagnóstico foi confirmado visualmente no Poco C85. Isso valida apenas a inicialização básica do renderer de diagnóstico, não a renderização de jogos.
- **Memória:** RDRAM de 8 MiB, SP DMEM de 4 KiB e SP IMEM de 4 KiB, com acessos big-endian e verificações de limites.
- **Barramento físico:** decodifica RDRAM, DMEM, IMEM, registradores MMIO iniciais do MI e do SP, incluindo SP_PC, SP_STATUS e registradores de DMA.
- **DMA do SP:** transferência RDRAM↔SP, com linhas, contagem, skip, alternância DMEM/IMEM e wrap de endereço SP. A implementação atual executa a cópia de forma síncrona; temporização e fila de DMA ainda não estão modeladas.
- **MI / interrupções:** registradores iniciais de modo, versão, status e máscara; o barramento expõe interrupções pendentes e o SP pode sinalizar interrupção ao concluir BREAK quando habilitado.
- **CPU principal:** interpretador MIPS III inicial com GPR, aritmética/lógica, saltos, desvios/delay slots, loads/stores, CP0, exceções e suporte inicial a TLB. A CPU e o RSP compartilham o barramento na classe `N64Machine`.
- **RSP escalar:** interpretador inicial com subconjunto de instruções escalares, acesso a DMEM/IMEM, desvios, delay slots, BREAK e controle básico de execução por SP_PC/SP_STATUS.
- **RSP COP2:** movimentos MFC2/MTC2/CFC2/CTC2 e subconjunto de operações vetoriais: VADD, VSUB, VADDC, VSUBC e operações lógicas VAND/VNAND/VOR/VNOR/VXOR/VNXOR. Não representa uma unidade vetorial completa.
- **Testes:** testes nativos host via CMake/CTest para barramento, CPU/RSP e máquina compartilhada; autotestes JNI adicionais são executados na inicialização do app e reportam resultados no Logcat com a tag `M64plus-Native`.

## Limitações importantes

Ainda não existe boot completo de ROM nem compatibilidade comprovada com jogos. O RSP é parcial: faltam grande parte das instruções vetoriais, loads/stores vetoriais, pipeline/hazards e microcodes HLE. Também faltam os demais dispositivos do console (PI, VI, AI, RI, SI), cartucho/ROM loader, RDP, áudio, FPU, caches e um agendador fiel em ciclos. A apresentação de jogos por um renderer N64 ainda não está implementada.

A documentação técnica descreve a pesquisa do Mupen64Plus AE como referência arquitetural; ela não significa que o código do AE tenha sido incorporado. O projeto antigo `N64-Android` permanece independente.

## Build e testes

O workflow do GitHub Actions executa:
1. configuração e compilação dos testes nativos com CMake;
2. execução dos testes com CTest;
3. compilação do APK debug com Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1.

Os testes host não substituem os autotestes JNI no aparelho. Para confirmar estes últimos, instale e abra o APK no dispositivo e consulte o Logcat. O build do APK, por si só, não comprova execução de jogos nem validação no Poco C85.

## Próximas etapas sugeridas

1. Expandir e validar COP2 com testes de referência para cada operação e seus flags/accumulator.
2. Implementar as instruções de transferência vetorial do RSP e ampliar o subconjunto escalar.
3. Modelar corretamente sincronização, interrupções, DMA e orçamento de ciclos entre CPU e RSP.
4. Implementar PI/cartucho e um carregador de ROM de teste.
5. Só então avançar para tarefas gráficas RSP, RDP e integração de renderer, validando EGL/Surface separadamente.
