# M64plus

Emulador Nintendo 64 para Android. Alvo inicial: Android 16 ARM64 e testes no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

**Gráfico:** aplicativo Android com OpenGL ES 2.0; triângulo de diagnóstico confirmado visualmente no Poco C85.

**RDRAM:** memória principal de 8 MiB em C++17, com acesso big-endian de 8, 16, 32 e 64 bits, reset e proteção de limites. Autoteste JNI na inicialização.

**CPU MIPS inicial:** 32 GPRs de 64 bits (r0 sempre zero), PC de 64 bits, subconjunto aritmético/lógico inicial e agora saltos/desvios: BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ, J, JAL, JR e JALR, além de LB, LBU, LH, LHU, LW, LWU, LD, SB, SH, SW e SD. A CPU modela uma instrução de delay slot. Os acessos de dados usam offsets diretos da RDRAM de 8 MiB, com checagem de alinhamento e limites; MMU/TLB e exceções ainda não estão implementados. Autotestes cobrem desvios, delay slot, leitura e escrita.

**CP0 inicial:** registradores BadVAddr, Count, Compare, Status, Cause e EPC; captura de exceção, bit EXL, indicação de delay slot (BD), vetor normal/BEV, retorno ERET e temporizador Count/Compare. Este CP0 inicial ainda não está ligado ao fluxo de execução da CPU MIPS; TLB, modos de privilégio completos e arbitragem real de interrupções ainda faltam.

**Limitações:** ainda não há exceções completas integradas à CPU, TLB, FPU, caches, barramento completo, RSP, RDP, áudio ou carregador de ROM. Ainda não executa jogos comerciais. Instruções de branch-likely e algumas variantes não estão implementadas.

## Build e testes

GitHub Actions compila o APK debug com Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1. Na inicialização, autotestes nativos registram resultado no Logcat sob a tag M64plus-Native.

## Próximas etapas

1. Validar a compilação e os autotestes no CI e no Poco C85.
2. Implementar loads/stores e o barramento de memória.
3. Integrar CP0 à CPU, desenvolver exceções arquiteturais e TLB.
4. Completar instruções MIPS III, FPU e caches.
5. Implementar RSP, RDP, áudio, controles e carregamento de ROMs.

Referência técnica principal: manual do NEC VR4300 (arquitetura MIPS III).
