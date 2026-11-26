# M64plus

Emulador Nintendo 64 para Android. Alvo inicial: Android 16 ARM64 e testes no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

- **Gráficos:** aplicativo Android com OpenGL ES 2.0. O triângulo de diagnóstico foi confirmado visualmente no Poco C85. O núcleo RDP inicial já decodifica comandos, mantém estado de imagem/tiles/modos, carrega texturas para TMEM (incluindo formatos de 4/8/16/32 bits e bancos RGBA32) e rasteriza operações limitadas de preenchimento, triângulos flat-fill e texture rectangles RGBA16 normais e com flip, com amostragem nearest.
- **RDRAM:** memória de 8 MiB em C++17, big-endian, com acessos de 8/16/32/64 bits e proteção de limites.
- **Barramento físico parcial:** decodifica RDRAM (8 MiB), SP DMEM/IMEM (4 KiB cada), cartucho ROM e registradores MMIO iniciais de MI, SP, DP e PI. Inclui acessos big-endian, leitura de comandos DP por RDRAM ou XBUS/DMEM, DMA SP↔RDRAM com múltiplas linhas e skip, e DMA PI de cartucho para RDRAM (`PI_WR_LEN`), com validação de limites e interrupção MI. O cartucho `.z64` pode ser carregado no barramento; ainda não há fluxo completo de boot nem todos os dispositivos.
- **CPU:** núcleo MIPS III inicial com 32 registradores GPR, aritmética/lógica, saltos, desvios e delay slots. Inclui LB/LBU/LH/LHU/LW/LWU/LD e SB/SH/SW/SD.
- **RSP inicial:** interpretador escalar parcial com execução pela IMEM, registradores GPR, branches/delay slots, loads/stores escalares, transferências COP0 para registradores SP/DP, e unidade vetorial COP2 parcial. Há operações vetoriais aritméticas/lógicas, multiplicação/acumulador, comparações e parte dos formatos de transferência vetorial entre registradores e DMEM (incluindo LQV/LRV, LPV/LUV, LHV/SHV, LFV/SFV, LTV/STV e SWV). Não é ainda um RSP completo: faltam instruções vetoriais e semântica de pipeline/hazards.
- **CP0:** registradores e operações iniciais para exceções, EPC, Cause, BadVAddr, Count/Compare, ERET e operações TLB.
- **TLB:** 32 entradas, páginas de 4 KiB em pares de 8 KiB, ASID, global, válido e dirty. Implementa TLBP, TLBR, TLBWI e TLBWR. A CPU pode usar a tradução em busca de instruções e loads/stores; KSEG0/KSEG1 usam mapeamento direto.
- **Testes nativos:** autotestes JNI executados na inicialização e resultados enviados ao Logcat com a tag `M64plus-Native`. Cobrem RDRAM, CPU, desvios, CP0, instruções TLB, tradução virtual, falhas de mapeamento, proteção de escrita, barramento/MMIO inicial, SP DMEM/IMEM, DMA SP e PI, leitura de comandos DP, integração CPU-barramento e instruções escalares/vetoriais selecionadas do RSP.

## Limitações importantes

A tradução TLB continua opt-in e ainda não existe um fluxo completo de boot do N64. O barramento/MMIO implementado é parcial: VI, AI, SI, RI, RDRAM registers e diversos dispositivos auxiliares ainda não estão mapeados; o DMA PI atual cobre somente leitura de cartucho ROM para RDRAM, sem dispositivos de armazenamento graváveis. Também faltam modos de privilégio completos, FPU, caches, RSP completo (conjunto COP2 ainda parcial, instruções restantes e comportamento de pipeline/hazards), rasterização RDP geral com combinação/Z-buffer, áudio, integração do carregamento de ROM ao fluxo de boot e compatibilidade com jogos. O RDP atual é apenas uma base parcial: texture rectangles suportam os opcodes TEXTURE_RECTANGLE e TEXTURE_RECTANGLE_FLIP, imagens e tiles RGBA16, coordenadas alinhadas, derivadas unitárias, clamp S/T e amostragem nearest; não há filtragem bilinear, masks/mirror, combinação de cores ou suporte geral aos demais formatos e modos. Os triângulos rasterizados estão limitados ao caminho flat-fill sem shade, textura ou Z. O triângulo confirma somente a inicialização básica do OpenGL ES; não significa que jogos já executam.

## Build

GitHub Actions compila o APK debug com Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1. A compilação do APK não executa automaticamente os autotestes JNI no aparelho; para confirmar os resultados deles, é necessário instalar e abrir o aplicativo e consultar o Logcat.

## Próximas etapas

1. Validar os autotestes JNI no Poco C85.
2. Completar os dispositivos MMIO do barramento e conectar a CPU ao barramento físico do VR4300.
3. Ampliar o conjunto de instruções MIPS III e implementar FPU/caches.
4. Ampliar RSP/RDP (mais formatos e modos de textura, filtragem, shade e Z), áudio, controles e carregamento de ROM.
