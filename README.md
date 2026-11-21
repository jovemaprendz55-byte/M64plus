# M64plus

Emulador Nintendo 64 para Android. Alvo inicial: Android 16 ARM64 e testes no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

**Gráfico:** aplicativo Android com OpenGL ES 2.0; triângulo de diagnóstico confirmado visualmente no Poco C85.

**RDRAM:** memória principal de 8 MiB em C++17, com acesso big-endian de 8, 16, 32 e 64 bits, reset e proteção de limites. Autoteste JNI na inicialização.

**CPU MIPS inicial:** 32 GPRs de 64 bits (r0 sempre zero), PC de 64 bits, subconjunto aritmético/lógico inicial e agora saltos/desvios: BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ, J, JAL, JR e JALR, além de LB, LBU, LH, LHU, LW, LWU, LD, SB, SH, SW e SD. A CPU modela uma instrução de delay slot. Os acessos de dados usam offsets diretos da RDRAM de 8 MiB, com checagem de alinhamento e limites; MMU/TLB e exceções ainda não estão implementados. Autotestes cobrem desvios, delay slot, leitura e escrita.

**CP0 inicial:** registradores BadVAddr, Count, Compare, Status, Cause e EPC; captura de exceção, bit EXL, indicação de delay slot (BD), vetor normal/BEV, retorno ERET e temporizador Count/Compare. O CP0 agora está conectado ao passo da CPU para exceções de instrução reservada, busca inválida, endereços de dados desalinhados e falhas de acesso à RDRAM. A CPU registra EPC/Cause/BadVAddr e desvia para o vetor de exceção. TLB, modos de privilégio completos e arbitragem real de interrupções ainda faltam.

**TLB inicial:** tabela associativa de 32 entradas, páginas de 4 KiB em pares de 8 KiB, ASID, páginas globais, bits válido/dirty, falha de tradução, página inválida e proteção de escrita. KSEG0/KSEG1 usam mapeamento direto. As instruções COP0 TLBP, TLBR, TLBWI e TLBWR agora usam os registradores CP0 e a tabela TLB; Random percorre os índices não fixados por Wired. A tradução ainda não está conectada ao fetch e aos loads/stores da CPU.\n\n**Limitações:** ainda não há exceções completas para todos os casos, integração da TLB com a CPU, FPU, caches, barramento completo, RSP, RDP, áudio ou carregador de ROM. Ainda não executa jogos comerciais. Instruções de branch-likely e algumas variantes não estão implementadas.

## Build e testes

GitHub Actions compila o APK debug com Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1. Na inicialização, autotestes nativos registram resultado no Logcat sob a tag M64plus-Native.

## Próximas etapas

1. Validar a compilação e os autotestes no CI e no Poco C85.
2. Implementar loads/stores e o barramento de memória.
3. Integrar CP0 à CPU, desenvolver exceções arquiteturais e TLB.
4. Completar instruções MIPS III, FPU e caches.
5. Implementar RSP, RDP, áudio, controles e carregamento de ROMs.

Referência técnica principal: manual do NEC VR4300 (arquitetura MIPS III).


### CP0 TLB register and entry inspection foundation
- Added CP0 register slots and write masks for Index, Random, EntryLo0/1, Context, PageMask, Wired, EntryHi and XContext.
- Random is read-only in this initial model; writing Wired resets Random to 31.
- PageMask is deliberately fixed to zero because the current TLB supports only 4 KiB pages.
- Added TLB entry readback and associative probe helpers, covering the behavior needed by future TLBR/TLBP instructions.
- JNI self-tests cover register masks, read-only Random, probe ASID/global matching, and entry readback.
- Not yet implemented: TLBP/TLBR/TLBWI/TLBWR instruction decoding or virtual-memory integration into CPU fetch/load/store.

### COP0 TLB instructions
- Implemented TLBP, TLBR, TLBWI and TLBWR in the CPU COP0 decoder.
- Connected Index, Random, EntryHi, EntryLo0/1 and Wired to the TLB operations.
- Random cycles downward through non-wired entries after each CPU instruction.
- Added an end-to-end native CPU test for TLBP hit/miss, TLBR readback, TLBWI and deterministic TLBWR.
- TLB translation is not yet connected to instruction fetch or data loads/stores; privilege checks and full MIPS III MMU behavior remain future work.
