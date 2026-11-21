# M64plus

Emulador Nintendo 64 para Android. Alvo inicial: Android 16 ARM64 e testes no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

**Marco 0 — gráfico:** aplicativo Android com OpenGL ES 2.0. O triângulo de diagnóstico foi confirmado visualmente no Poco C85.

**Marco 1 — RDRAM nativa:** implementada em C++17 uma memória de 8 MiB com leitura/escrita big-endian de 8, 16, 32 e 64 bits, reset e validação de limites. O aplicativo chama um autoteste JNI na inicialização e registra PASS ou FAIL no Logcat.

O autoteste verifica tamanho, leitura/escrita de uma palavra, ordem big-endian e rejeição de acesso que ultrapassa o fim da memória. Não é ainda uma implementação completa do subsistema de memória do Nintendo 64.

Ainda não há CPU MIPS/R4300, RSP, RDP, áudio, carregador de ROM ou emulação de jogos.

## Compilação

Requisitos: JDK 17, Gradle 8.13, Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1. O GitHub Actions compila o APK debug e publica o artefato m64plus-debug-apk.

## Próximas etapas

1. Validar o build nativo no CI e no Poco C85.
2. Ampliar os testes de memória, alinhamento e endianness.
3. Implementar CPU MIPS/R4300: registradores, PC, HI/LO e decodificação.
4. Implementar barramento e exceções.
5. Desenvolver RSP, RDP, áudio, controles e carregamento de ROMs.

O repositório antigo N64-Android permanece independente e intocado.
