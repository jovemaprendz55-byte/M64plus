# M64plus

Emulador Nintendo 64 para Android. Alvo inicial: Android 16 ARM64 e testes no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

- **Gráficos:** aplicativo Android com OpenGL ES 2.0. O triângulo de diagnóstico foi confirmado visualmente no Poco C85.
- **RDRAM:** memória de 8 MiB em C++17, big-endian, com acessos de 8/16/32/64 bits e proteção de limites.
- **Barramento físico inicial:** decodifica RDRAM, SP DMEM (4 KiB) e SP IMEM (4 KiB); acessos big-endian e rejeição segura de regiões ainda não mapeadas. A CPU já pode buscar instruções e executar loads/stores usando o barramento; MMIO e DMA ainda faltam.
- **CPU:** núcleo MIPS III inicial com 32 registradores GPR, aritmética/lógica, saltos, desvios e delay slots. Inclui LB/LBU/LH/LHU/LW/LWU/LD e SB/SH/SW/SD.
- **CP0:** registradores e operações iniciais para exceções, EPC, Cause, BadVAddr, Count/Compare, ERET e operações TLB.
- **TLB:** 32 entradas, páginas de 4 KiB em pares de 8 KiB, ASID, global, válido e dirty. Implementa TLBP, TLBR, TLBWI e TLBWR. A CPU pode usar a tradução em busca de instruções e loads/stores; KSEG0/KSEG1 usam mapeamento direto.
- **Testes nativos:** autotestes JNI executados na inicialização e resultados enviados ao Logcat com a tag `M64plus-Native`. Cobrem RDRAM, CPU, desvios, CP0, instruções TLB, tradução virtual, falha de mapeamento, página inválida, proteção de escrita e decodificação do barramento/SP DMEM/IMEM e integração CPU-barramento com execução de código na IMEM e dados na DMEM.

## Limitações importantes

A tradução TLB continua opt-in e ainda não existe um fluxo completo de boot do N64. Faltam, entre outros componentes, o barramento completo e mapeamento de dispositivos, modos de privilégio completos, FPU, caches, RSP, RDP, áudio, carregamento de ROM e compatibilidade com jogos. O triângulo confirma somente a inicialização básica do OpenGL ES; não significa que jogos já executam.

## Build

GitHub Actions compila o APK debug com Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1. A compilação do APK não executa automaticamente os autotestes JNI no aparelho; para confirmar os resultados deles, é necessário instalar e abrir o aplicativo e consultar o Logcat.

## Próximas etapas

1. Validar os autotestes JNI no Poco C85.
2. Completar os dispositivos MMIO do barramento e conectar a CPU ao barramento físico do VR4300.
3. Ampliar o conjunto de instruções MIPS III e implementar FPU/caches.
4. Desenvolver RSP, RDP, áudio, controles e carregamento de ROM.
