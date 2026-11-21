# M64plus

Emulador Nintendo 64 para Android. Alvo inicial: Android 16 ARM64 e testes no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

**Gráfico:** aplicativo Android com OpenGL ES 2.0; triângulo de diagnóstico confirmado visualmente no Poco C85.

**RDRAM:** memória principal de 8 MiB em C++17, com acesso big-endian de 8, 16, 32 e 64 bits, reset e proteção de limites. Autoteste JNI na inicialização.

**CPU MIPS inicial:** esqueleto com 32 registradores GPR de 64 bits (r0 sempre zero), PC de 64 bits e subconjunto de instruções: SLL, SRL, ADDU, SUBU, AND, OR, XOR, NOR, SLT, SLTU, DADDU, DSUBU, ADDIU, DADDIU, ANDI, ORI, XORI e LUI. Autoteste executa um programa de seis instruções sobre a RDRAM.

**Limitações:** ainda não há branch/jump e delay slots, exceções, CP0/TLB, FPU, caches, barramento completo, RSP, RDP, áudio ou carregador de ROM. Ainda não executa jogos comerciais.

## Build e testes

GitHub Actions compila o APK debug com Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1. Na inicialização, autotestes nativos registram resultado no Logcat sob a tag M64plus-Native.

## Próximas etapas

1. Confirmar build e autotestes no CI e no Poco C85.
2. Implementar branch/jump com delay slot e exceções.
3. Desenvolver CP0, mapa de memória e barramento.
4. Completar instruções MIPS III, FPU e caches.
5. Implementar RSP, RDP, áudio, controles e carregamento de ROMs.

Referência técnica principal: manual do NEC VR4300 (arquitetura MIPS III).
