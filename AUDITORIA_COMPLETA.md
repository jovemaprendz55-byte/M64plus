# Auditoria técnica — M64plus

**Repositório:** `jovemaprendz55-byte/M64plus`  
**Branch de registro:** `main`  
**Escopo:** auditoria estática e revisão das implementações já examinadas.  
**Modo:** somente documentação; nenhuma correção de código foi aplicada.  
**Status:** auditoria em andamento — este documento consolida os resultados conhecidos até 2026-11-21.

> Importante: “ausente” significa que não foi encontrada implementação nos arquivos examinados; não equivale a teste em hardware. “Defeito confirmado” refere-se a comportamento identificado no código e confrontado com a arquitetura/documentação. Pontos não resolvidos ficam marcados como investigação pendente.

## 1. Resumo executivo

O projeto já possui uma base inicial de emulação:
- memória RDRAM de 8 MiB, big-endian, com leituras/escritas de 8, 16, 32 e 64 bits;
- barramento com RDRAM, SP DMEM/IMEM e parte dos registradores MMIO MI/SP;
- DMA inicial do SP;
- CPU MIPS III/VR4300 parcial;
- RSP escalar e operações vetoriais COP2 parciais;
- integração Android com OpenGL ES 2.0 e triângulo de diagnóstico;
- testes nativos e workflow de CI para build/testes.

Ainda não é um emulador capaz de executar jogos comerciais de ponta a ponta. Faltam componentes essenciais, incluindo FPU/CP1, grande parte do conjunto de instruções, PI/cartucho e carregamento de ROM, RDP, áudio e diversos periféricos, além de temporização e inicialização completas.

## 2. Classificação dos achados

- **Implementado:** há implementação identificada; isso não garante fidelidade total.
- **Parcial:** existe suporte, mas faltam casos ou fidelidade.
- **Ausente:** não foi encontrada implementação nos arquivos examinados.
- **Defeito confirmado:** discrepância identificada no código.
- **Pendente de investigação:** fontes ou comportamento ainda não permitem conclusão segura.

## 3. CPU MIPS III / VR4300

### 3.1 Implementado ou parcialmente implementado
- Decodificação e execução de um subconjunto de instruções MIPS.
- Parte do CP0 e do TLB.
- Exceções com EPC e Cause.BD para instruções em delay slot.
- Desvios comuns BEQ/BNE/BLEZ/BGTZ e BLTZAL/BGEZAL.
- Tradução TLB com mapeamento direto KSEG0/KSEG1 implementada na classe TLB.
- ADDIU revisado: a implementação atual soma em 32 bits e faz sign-extension; o achado anterior de erro foi corrigido na análise e **não deve ser tratado como defeito**.

### 3.2 Ausente ou incompleto
- SRA, SLLV, SRLV, SRAV e várias operações de deslocamento de 64 bits.
- MULT/MULTU/DIV/DIVU, operações de 64 bits DMULT/DDIV e gerenciamento completo de HI/LO.
- ADD/ADDI/SUB/DADD/DSUB com exceções de overflow.
- LWL/LWR/SWL/SWR.
- LDL/LDR/SDL/SDR.
- LL/SC e mecanismo de reservation/LLbit.
- Instruções branch-likely: BEQL, BNEL, BLEZL, BGTZL, BLTZL, BGEZL, BLTZALL e BGEZALL.
- SYSCALL e BREAK da CPU, além de outras instruções do conjunto.
- COP1/FPU e suas instruções de memória/transferência/operações.
- Verificações completas de modos de privilégio e extensões de 64 bits.

### 3.3 Defeitos e lacunas confirmados
1. **Mapeamento KSEG0/KSEG1 — CORRIGIDO em 2026-11-21:** a CPU agora aplica a tradução direta desses segmentos mesmo quando `tlbEnabled_` está desativado, tanto na busca de instruções quanto em loads/stores. Foi adicionado teste de regressão que executa código via KSEG0 e grava em memória via KSEG1. Commits: `b71d413` (correção) e `f83f4a0` (teste). O workflow Android foi iniciado; resultado ainda pendente no momento desta atualização.
2. **JALR verifica alinhamento cedo demais:** o alvo desalinhado gera Address Load antes de executar o delay slot; a exceção pode ter EPC/BD incorretos em relação ao fluxo arquitetural esperado.
3. **Prioridade de exceções de endereço:** em determinados acessos desalinhados, a tradução é tentada antes da checagem de alinhamento, podendo produzir TLB fault onde deveria ocorrer Address Error.
4. **Interrupções ignoram ERL:** a verificação de interrupção não considera o bit ERL do Status.
5. **Exceção TLB aninhada:** Context/EntryHi/XContext não são atualizados em todos os casos quando EXL já está ativo.
6. **XContext incompleto/incorreto:** máscara e atualização não refletem integralmente os campos arquiteturais; precisa revisão específica.
7. **ERET parcial:** retorna por EPC e limpa EXL, mas não trata ERL/ErrorEPC.
8. **Contagem CP0 aproximada:** Count avança por instrução executada, não por temporização fiel do VR4300.
9. **Branch-likely ausente:** além da decodificação, é necessário implementar o anulamento do delay slot quando o branch-likely não é tomado.

### 3.4 Observações que não devem ser classificadas como defeitos confirmados
- Branch dentro de delay slot é comportamento arquitetural imprevisível; o tratamento atual como RI não deve ser marcado como bug confirmado sem uma política explícita de compatibilidade.
- J/JAL do RSP usa alvo que parece não mascarar localmente os bits do PC, mas o caminho integrado sincroniza e mascara o PC pelo barramento; não foi confirmado defeito no caminho integrado.
- A lógica de EPC/BD para exceções em delay slot foi considerada correta nos casos examinados.

## 4. CP0 e TLB

### Implementado parcialmente
- 32 entradas TLB.
- TLBP, TLBR, TLBWI e TLBWR.
- Exceções TLB básicas.
- Count/Compare e ERET parciais.
- PageMask fixo em zero (páginas de 4 KiB).

### Lacunas
- PageMask não suporta páginas maiores.
- XContext não está corretamente implementado.
- ERET não cobre retorno por ERL/ErrorEPC.
- Reset a frio não inicializa integralmente ERL/BEV como esperado para o estado de boot.
- Não há testes dedicados abrangentes de CP0/MIPS/TLB.
- Falta verificar KSU/CU0/CU1, permissões por modo e KX/SX/UX para instruções/endereços de 64 bits.

## 5. COP1 / FPU

**Status: não encontrada implementação nos arquivos examinados.**

A auditoria identificou ausência de suporte visível para:
- banco de registradores FPR e modos FR;
- FCR0/FCR31;
- MFC1/MTC1/CFC1/CTC1;
- LWC1/SWC1/LDC1/SDC1;
- operações aritméticas, raiz quadrada, conversões e comparações;
- BC1T/BC1F;
- arredondamento IEEE 754 e exceções de ponto flutuante;
- verificação de CU1 e geração correta de Coprocessor Unusable.

A conclusão deve ser revalidada contra a árvore completa do repositório antes de declarar que não existe qualquer arquivo FPU fora dos arquivos já inspecionados.

## 6. RSP / COP2

### 6.1 Estado e operações
- 32 registradores vetoriais, cada um com 8 lanes de 16 bits.
- VCO, VCC e VCE parcialmente representados.
- MFC2/MTC2 e CFC2/CTC2 parciais.
- Operações vetoriais encontradas: VADD, VSUB, VADDC, VSUBC, VAND, VNAND, VOR, VNOR, VXOR e VNXOR.
- Multiplicação vetorial e outras instruções COP2 ainda ausentes.

### 6.2 Acumulador
O RSP precisa de oito acumuladores de 48 bits, divididos em ACC_HI, ACC_MD e ACC_LO. A implementação atual usa valores de 64 bits, mas as operações examinadas substituem o acumulador inteiro em casos nos quais deveriam atualizar apenas ACC_LO e preservar as demais fatias. VADDC também não deve guardar o carry como parte do valor de ACC_LO.

Consequência: VADD/VSUB, VADDC/VSUBC e operações lógicas podem corromper partes do acumulador.

### 6.3 VSAR/VSAW
A codificação e os elementos usados para acessar as fatias do acumulador ainda precisam de confirmação. As referências consultadas apresentam convenções conflitantes para a seleção HI/MD/LO. Não implementar com base em uma única interpretação sem validação adicional.

### 6.4 BREAK e fluxo de execução
- BREAK do RSP integrado à máquina sinaliza SP_HALT/SP_BROKE e pode gerar interrupção MI conforme INTR_BREAK; não classificar isso como defeito confirmado.
- Instrução RSP não suportada pode deixar o processador parado. Em chamadas posteriores, `N64Machine::stepRsp()` pode limpar o estado de halt ao sincronizar PC e tentar novamente a mesma instrução, causando repetição do erro. Revisar o contrato de erro/parada.

### 6.5 Multiplicação vetorial pendente
Ainda precisam ser implementados e testados VMULF/U, VMACF/U, VMUDL/M/N/H e VMADL/M/N/H, com semântica de produto, acumulação, saturação e fatias do acumulador conforme documentação oficial.

## 7. Barramento, memória e periféricos

### 7.1 RDRAM
- 8 MiB, big-endian.
- Leituras/escritas de 8/16/32/64 bits.
- Verificação de limites.
- Nenhum defeito específico da camada RDRAM foi confirmado nos arquivos examinados.

### 7.2 Bus e DMA SP
- Mapeamento inicial de RDRAM, SP DMEM/IMEM, registradores MI e SP.
- DMA SP em ambas as direções, com linhas/skip, seleção de banco IMEM, wrap e rejeição de acesso fora dos limites.
- DMA é síncrono e não reproduz a temporização real.
- MMIO de 32 bits exige alinhamento nos caminhos examinados.

### 7.3 Defeitos identificados
1. **MI_VERSION:** constante atual `0x01010101`; valor esperado para o N64 é `0x02020102`.
2. **MI_MODE:** implementação faz atribuição simples com máscara, sem interpretar corretamente os bits de comando para limpar/definir modos e limpar DP interrupt.
3. **SP_STATUS:** comandos de escrita estão associados a bits incorretos para SINGLE STEP e INTERRUPT ON BREAK; comandos para SIG0..SIG7 também não foram implementados integralmente.

### 7.4 Periféricos ausentes
- PI e interface/cartucho.
- Carregamento e mapeamento de ROM.
- VI.
- AI e áudio.
- SI e controles.
- RI.
- Implementação funcional do RDP e registradores DP.
- Temporização e sincronização entre periféricos.

## 8. Inicialização, máquina e renderização Android

- Android inicializa OpenGL ES 2.0 e desenha um triângulo de diagnóstico.
- A máquina N64 e o RSP têm testes nativos, mas a execução integrada da máquina ainda não está conectada ao loop de renderização para produzir frames de jogos.
- MainActivity executa autotestes nativos e registra PASS/FAIL, mas a interface não apresenta um painel de diagnóstico completo.
- A aplicação ainda não demonstra carregamento de ROM e renderização de um jogo N64.
- Boot completo do console/cartucho ainda não foi implementado.

**Conclusão:** o triângulo de diagnóstico comprova o caminho básico de OpenGL, não a emulação de gráficos N64.

## 9. Testes e CI

Arquivos de teste identificados:
- `tests/bus_tests.cpp`
- `tests/machine_tests.cpp`
- `tests/rsp_cpu_tests.cpp`

Lacunas:
- não foram encontrados testes dedicados abrangentes para CP0, TLB e decodificação MIPS;
- testes RSP não cobrem completamente todas as flags e fatias do acumulador;
- CI compila e executa testes nativos configurados, mas não comprova execução real em dispositivo, carregamento de ROM ou renderização de jogos;
- autotestes JNI executados no aplicativo não são substitutos para testes automatizados de compatibilidade de ROM.

Configuração CI anteriormente examinada: Android SDK 36, NDK 27.2.12479018 e CMake 3.22.1.

## 10. Licenciamento e referências externas

Não foi encontrado arquivo LICENSE na árvore/branch examinada durante a auditoria. Definir uma licença própria para o projeto e revisar a licença de qualquer código externo antes de incorporá-lo. Mupen64Plus deve permanecer referência técnica; não copiar código GPL sem avaliação explícita de compatibilidade e obrigações.

## 11. Prioridades recomendadas

### P0 — Correção de base
1. Corrigir tradução direta KSEG0/KSEG1 quando TLB está desabilitado.
2. Corrigir comandos e bits de MI_MODE, MI_VERSION e SP_STATUS.
3. Corrigir ordem de exceções de alinhamento e fluxo de JALR.
4. Corrigir o modelo de acumulador RSP de 48 bits e preservar ACC_HI/MD nas operações.
5. Evitar repetição infinita de instrução RSP não suportada.

### P1 — CPU e RSP
1. Completar instruções básicas MIPS, HI/LO e operações de 64 bits.
2. Implementar LWL/LWR/SWL/SWR e LDL/LDR/SDL/SDR.
3. Implementar LL/SC e reservation.
4. Completar CP0/TLB, modos de privilégio, CU0/CU1 e exceções.
5. Completar COP2, incluindo multiplicação e saturação vetorial.

### P2 — Funcionalidade de console
1. Implementar PI/cartucho e carregador de ROM.
2. Implementar boot e mapeamento de memória do cartucho.
3. Implementar RDP e integrar o pipeline gráfico à tela Android.
4. Implementar VI, AI, SI, RI e áudio.
5. Implementar temporização e sincronização.

### P3 — Validação
1. Criar testes unitários por instrução e por periférico.
2. Adicionar ROMs homebrew de teste e testes de regressão.
3. Validar em dispositivo Android real.
4. Medir compatibilidade e desempenho por jogo, sem alegar suporte antes dos testes.

## 12. Arquivos examinados até esta consolidação

Entre os arquivos já revisados estão:
- `app/src/main/cpp/n64/rsp_cpu.h` e `rsp_cpu.cpp`
- `app/src/main/cpp/n64/bus.h` e `bus.cpp`
- `app/src/main/cpp/n64/mips_cpu.h` e `mips_cpu.cpp`
- `app/src/main/cpp/n64/cp0.h` e `cp0.cpp`
- `app/src/main/cpp/n64/tlb.h` e `tlb.cpp`
- `app/src/main/cpp/n64/machine.h` e `machine.cpp`
- `app/src/main/cpp/n64/rdram.h` e `rdram.cpp`
- `app/src/main/cpp/n64/native-lib.cpp`
- `app/src/main/cpp/rsp-machine-jni.cpp`
- `app/src/main/java/com/jsr/m64plus/MainActivity.java`
- `app/src/main/java/com/jsr/m64plus/BootRenderer.java`
- `tests/bus_tests.cpp`
- `tests/machine_tests.cpp`
- `tests/rsp_cpu_tests.cpp`
- arquivos CMake e workflow de CI.

## 13. Próximas áreas da auditoria

A auditoria ainda deve continuar por:
- árvore completa do projeto para confirmar arquivos ainda não examinados;
- carregador de ROM, formato de cabeçalho, byte swapping e mapeamento PI;
- boot sequence, CIC/ IPL3 e estado inicial da CPU;
- RDP: comandos, rasterização, TMEM, tiles, textura, blender e framebuffer;
- VI: modos de vídeo, framebuffer e sincronização com OpenGL;
- AI/áudio, SI/controle, PI e demais periféricos;
- integração ponta a ponta CPU → Bus → RSP/RDP → VI → tela;
- testes de regressão e compatibilidade.

---

**Nota de manutenção:** atualizar este documento após cada nova etapa da auditoria. Não marcar uma funcionalidade como concluída apenas por compilar; exigir teste direcionado e evidência de comportamento.