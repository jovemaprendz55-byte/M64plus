# Auditoria técnica — M64plus

**Repositório:** `jovemaprendz55-byte/M64plus`  
**Branch de registro:** `main`  
**Escopo:** auditoria estática e revisão das implementações já examinadas.  
**Modo:** auditoria contínua com correções registradas diretamente na `main`.  
**Status:** auditoria em andamento — este documento consolida os resultados conhecidos até 2026-11-22.

> Importante: “ausente” significa que não foi encontrada implementação nos arquivos examinados; não equivale a teste em hardware. “Defeito confirmado” refere-se a comportamento identificado no código e confrontado com a arquitetura/documentação. Pontos não resolvidos ficam marcados como investigação pendente.

## 1. Resumo executivo

O projeto já possui uma base inicial de emulação:
- memória RDRAM de 8 MiB, big-endian, com leituras/escritas de 8, 16, 32 e 64 bits;
- barramento com RDRAM, SP DMEM/IMEM e parte dos registradores MMIO MI/SP;
- DMA inicial do SP;
- CPU MIPS III/VR4300 parcial;
- RSP escalar e operações vetoriais COP2 parciais (incluindo VMULQ, VMACQ, VRNDP e VRNDN);
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
- Operações vetoriais encontradas: VADD, VSUB, VADDC, VSUBC, VLT, VEQ, VNE, VGE, VCH, VCR, VMRG, VAND, VNAND, VOR, VNOR, VXOR e VNXOR.
- VMULF/U, VMACF/U, VMUDL/M/N/H, VMADL/M/N/H e as comparações VLT/VEQ/VNE/VGE, os clips VCH/VCL/VCR e VMRG foram implementados e receberam testes direcionados; demais instruções COP2 continuam ausentes.

### 6.2 Acumulador
O RSP possui oito acumuladores de 48 bits, cada um representado por ACC_HI, ACC_MD e ACC_LO de 16 bits. A representação em três fatias já foi implementada. As operações vetoriais básicas examinadas atualizam ACC_LO sem substituir ACC_MD/HI; VADDC guarda carry em VCO, não como parte do valor de ACC_LO. VMULF/U substituem o acumulador com produto arredondado; VMACF/U somam o produto não arredondado ao acumulador existente, com wrap de 48 bits. VMUDL/M/N/H também foram adicionadas: VMUDL grava o produto unsigned >> 16 em ACC_LO; VMUDM grava produto signed×unsigned com extensão de sinal; VMUDN grava unsigned×signed com extensão de sinal; VMUDH grava signed×signed deslocado 16 bits. VMADL/M/N/H acumulam os respectivos produtos no acumulador de 48 bits; VMADL e VMADN usam saturação unsigned, VMADM e VMADH saturação signed.

### 6.3 VSAR/VSAW
VSAR (função 0x1D) está implementada: elemento 0 lê ACC_LO, elemento 1 lê ACC_MD e elemento 2 lê ACC_HI. Foram adicionados testes para as fatias e para os resultados de VMULF/VMACF. A cobertura de valores não-zero em ACC_HI e propagação de carry entre as três fatias ainda deve ser ampliada.

### 6.4 BREAK e fluxo de execução
- BREAK do RSP integrado à máquina sinaliza SP_HALT/SP_BROKE e pode gerar interrupção MI conforme INTR_BREAK; não classificar isso como defeito confirmado.
- **Falha de instrução RSP — CORRIGIDA em 2026-11-21:** `RspCpu` agora expõe estado de falha latched; instruções não suportadas e erros de execução não são repetidos automaticamente. `N64Machine::stepRsp()` mantém a falha enquanto o SP_PC não mudar; uma alteração explícita de SP_PC permite iniciar nova tarefa. BREAK normal permanece distinto de falha. Testes adicionados em `tests/rsp_cpu_tests.cpp` e `tests/machine_tests.cpp`. Commits: `2b40c00`, `185aeb3`, `44857a0`, `c627de9`, `c9b20c2`, `7ab4bee`, `209f004`.

### 6.5 Multiplicação vetorial pendente
VMULF/U, VMACF/U, VMUDL/M/N/H e VMADL/M/N/H foram implementadas. Permanecem pendentes a ampliação de testes de limites/saturação para todos os formatos, com semântica de produto, acumulação e fatias do acumulador conforme documentação oficial.

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
3. **SP_STATUS — CORRIGIDO em 2026-11-21:** comandos SINGLE STEP e INTERRUPT ON BREAK foram separados conforme os bits de comando; comandos de clear/set SIG0..SIG7 foram adicionados. Testes em `tests/bus_tests.cpp` cobrem set/clear e geração de interrupção SP. Commit de implementação/teste: `5dd8fbc`, `645c841`, `b8922b8`, `797cd92`. CI nativo e Android concluído com sucesso no run `44900046288`.

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
2. Corrigir comandos e bits de MI_MODE e MI_VERSION. SP_STATUS foi corrigido.
3. Corrigir ordem de exceções de alinhamento e fluxo de JALR.
4. Ampliar a validação do acumulador RSP de 48 bits e preservar ACC_HI/MD nas operações.
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

## 14. Registro de desenvolvimento — 2026-11-21

### Correções concluídas
- SP_STATUS: comandos SSTEP, INTR_BREAK e SIG0..SIG7 corrigidos; testes nativos e build Android passaram no workflow `44900046288`.
- RSP: estado de falha separado do BREAK normal; repetição automática de opcode não suportado bloqueada. A mudança inicial passou no workflow `44900266157`; revisão posterior corrigiu a distinção BREAK/falha e adicionou assertivas.
- Acumulador RSP: representação mudou de oito inteiros de 64 bits para oito conjuntos de três fatias de 16 bits; operações já suportadas atualizam ACC_LO; VSAR (função 0x1D) lê ACC_LO/MD/HI; testes adicionados.

### Commits do acumulador e revisão BREAK
- `0371153` — estrutura de fatias 48-bit.
- `978bd60` — preservação das fatias e implementação VSAR.
- `af99767` — testes ACC_LO/VSAR.
- `1183bab` — reset explícito das três fatias.
- `6b07cd2` — teste para garantir que BREAK normal não é marcado como falha.
- `209f004` — BREAK normal separado do estado de falha.

**Validação pendente:** executar workflow atualizado para os commits mais recentes do acumulador e confirmar native-host-tests e assemble-debug antes de marcar esta etapa como aprovada.

### Pendências técnicas do acumulador
- Testar preservação não-zero de ACC_HI e ACC_MD por meio de instruções que escrevam essas fatias.
- Implementar VMULF/U, VMACF/U, VMUDL/M/N/H e VMADL/M/N/H com semântica de 48 bits e saturação.
- Confirmar seletores VSAR contra a documentação de referência e testes direcionados.


### Continuação do RSP — VMULF/VMULU
- `VMULF` (função 0x00): produto fracionário assinado, arredondamento por 0x8000, acumulador de 48 bits e saturação assinada.
- `VMULU` (função 0x01): mesmo produto/accumulator, com saturação de saída sem sinal (negativo para zero e overflow positivo para 0xFFFF).
- Testes de regressão adicionados para produto positivo/negativo, caso extremo 0x8000 × 0x8000, saturação e leitura das fatias via VSAR.


### Continuação do RSP — VMACF/VMACU
- Implementadas as funções 0x08 (VMACF) e 0x09 (VMACU).
- Ambas multiplicam operandos assinados, dobram o produto e acumulam no valor anterior de 48 bits, com wrap de 48 bits.
- Diferente de VMULF/VMULU, VMACF/VMACU não adicionam o arredondamento 0x8000.
- VMACF retorna a janela ACC[31:16] com saturação assinada; VMACU retorna a mesma janela com saturação sem sinal (negativo para zero e overflow positivo para 0xFFFF).
- Testes adicionados para acumulação positiva/negativa, saturação, preservação de precisão baixa e leitura de ACC_MD via VSAR.
- Commits: `ea8b44b` (implementação) e `51d89d0` (testes).
- CI dos commits `ea8b44b` e `51d89d0` está em execução/na fila; aguardar os dois jobs antes de marcar como validado.


### Continuação do RSP — VMUDL/VMUDM/VMUDN/VMUDH
- Implementadas as funções 0x04–0x07 no método de execução vetorial do RSP.
- VMUDL calcula produto unsigned e descarta os 16 bits inferiores, armazenando o resultado em ACC_LO.
- VMUDM calcula signed(VS) × unsigned(VT), estende o produto para 48 bits e retorna a janela intermediária com saturação assinada.
- VMUDN calcula unsigned(VS) × signed(VT), estende o produto para 48 bits e aplica saturação unsigned na saída.
- VMUDH calcula signed(VS) × signed(VT), desloca o produto 16 bits à esquerda no acumulador e retorna a janela saturada.
- Adicionados testes dirigidos para produto, resultado, extensão de sinal e fatias VSAR.
- Commits: `f97028e` (implementação) e `0c9047e` (testes).
- Validação CI em andamento: workflow `44900883349`; não marcar como aprovado até ambos os jobs concluírem.


### Continuação do RSP — VMADL/VMADM/VMADN/VMADH
- Implementadas as funções 0x0C–0x0F para acumular os produtos parciais no acumulador de 48 bits.
- VMADL acumula produto unsigned >> 16 e aplica saturação unsigned na saída.
- VMADM acumula signed(VS) × unsigned(VT) e aplica saturação signed.
- VMADN acumula unsigned(VS) × signed(VT) e aplica saturação unsigned.
- VMADH acumula signed(VS) × signed(VT) deslocado 16 bits e aplica saturação signed.
- Corrigido o writeback de VMADL para saturar ACC[31:0], diferentemente do VMUDL que retorna ACC_LO.
- Testes adicionados com sequências VMUD→VMAD para os quatro formatos.
- Commits: `e759b8c` (implementação), `8434b6c` (correção VMADL) e `51f1c36` (testes).
- Validação do workflow `44900942783` pendente no momento deste registro.


### Continuação do RSP — comparações e seleção vetorial
- Implementadas VLT (0x20), VEQ (0x21), VNE (0x22), VGE (0x23) e VMRG (0x27).
- VLT trata empate usando VCO_LO e VCO_HI; VEQ/VNE consideram VCO_HI para os casos de empate; VGE aplica a regra complementar de VLT.
- As comparações escrevem as seleções em VCC_LO e limpam VCO; VMRG seleciona VS ou VT conforme VCC_LO.
- Adicionados testes para comparação signed, empates com flags VCO, VCC e seleção VMRG.
- Commits: `86d7d6a` (implementação), `9f8eda1` e `4325988` (testes/ajustes).
- CI do commit de testes `4325988` ainda deve concluir antes de marcar a etapa como aprovada.


### Revisão das comparações VLT/VEQ/VNE/VGE
- Revisada a semântica de flags com referência técnica: VLT usa VCO_LO e VCO_HI no empate; VEQ e VNE usam VCO_HI (não VCO_LO) no caso de igualdade.
- Corrigida a implementação de VEQ/VNE para consultar VCO_HI e garantido que comparações/VMRG atualizam ACC_LO com o valor selecionado.
- Testes ajustados para preparar explicitamente VCO_HI e validar as condições de empate.
- Commits: `c4f0a00` (correção) e `98369ee` (teste).


### Continuação do RSP — VCH e VCR
- Implementado VCH (função 0x25): detecta sinais opostos, gera VCO/VCC/VCE, calcula os limites de clipping em complemento de dois e grava o valor selecionado em ACC_LO/VD.
- Implementado VCR (função 0x26): variante de clipping que usa complemento de um para representar o limite negativo e limpa VCE.
- Testes direcionados cobrem limites positivos/negativos, sinais opostos, caso especial VCE e flags resultantes.
- Commits: `d6b70ed` (VCH), `cfb7271` (testes VCH), `0ff54b5` (VCR) e `2f4c43b` (testes VCR).
- CI das alterações de clipping ainda está em execução; aguardar `native-host-tests` e `assemble-debug` concluírem.


### Continuação do RSP — VCL
- Implementado VCL (função 0x24), etapa baixa do clipping de precisão dupla em conjunto com VCH.
- Reutiliza VCO/VCE gerados por VCH, atualiza seletivamente VCC_LO/VCC_HI, calcula o limite negativo em complemento de dois e grava o resultado em ACC_LO/VD.
- Testes cobrem a comparação em duas etapas: limite de sinal igual e caso oposto resolvido por VCE.
- Commits: `c287a04` (implementação) e `809c5ed` (testes).
- Validação CI pendente para a sequência VCH/VCL/VCR.


### Resultado final da validação — 2026-11-21
- Correção dos testes VCH/VCR: workflow `44901358345` concluído com sucesso nos dois jobs: `native-host-tests` e `assemble-debug`.
- Implementação/testes VCL e sequência de clipping: workflow `44901373926` concluído com sucesso nos dois jobs: `native-host-tests` e `assemble-debug`.
- Portanto, VCH, VCL e VCR estão implementados e a versão atual passou nos testes automatizados disponíveis. Isso não comprova compatibilidade com jogos nem validação em hardware real.
- Links de evidência:
  - https://github.com/jovemaprendz55-byte/M64plus/actions/runs/44901358345
  - https://github.com/jovemaprendz55-byte/M64plus/actions/runs/44901373926

### Próxima etapa técnica — instruções escalares/divisão do RSP
- Próximo grupo a implementar: VNOP (0x37), VMOV (0x33), VRCP (0x30), VRCPL (0x31), VRCPH (0x32), VRSQ (0x34), VRSQL (0x35) e VRSQH (0x36).
- Essas instruções não usam o caminho vetorial de oito lanes como as operações anteriores: selecionam um elemento escalar de VT e um elemento de destino de VD, codificado no campo VS.
- A documentação SGI/Nintendo descreve a seleção escalar, os campos de origem/destino e o protocolo de alta/baixa precisão: VRCPH/VRSQH armazenam a metade alta da entrada e devolvem a metade alta do resultado anterior; VRCPL/VRSQL completam a entrada de 32 bits. VMOV copia um elemento e VNOP não altera registradores.
- A implementação deve incluir estado explícito DIV_IN/DIV_OUT e indicador de entrada alta pendente, além do efeito colateral de carregar ACC_LO com VT quando especificado.
- A tabela de aproximação ROM do RSP precisa ser bit-exata; não substituir por divisão ou raiz quadrada de ponto flutuante. Implementar e testar a decodificação escalar/VMOV/VNOP primeiro, depois o cálculo da tabela e os pares H/L.
- Referências técnicas consultadas: SGI Nintendo 64 RSP Programmer's Guide, seção “VU Divide Instructions”, páginas 75–78; documentação técnica RSP de Dillonb e implementação de referência Project64.


### Continuação do RSP — VMOV/VNOP e unidade de divisão (2026-11-21)
- VMOV e VNOP implementados; os workflows `44901529621` e `44901532325` concluíram com sucesso em testes nativos e Android.
- Corrigido o mapeamento escalar do VMOV: origem e ACC_LO seguem o mapa de broadcast de elemento, não uma divisão simples do campo de elemento por dois.
- Implementada a unidade escalar de divisão do RSP:
  - VRCP (0x30), VRCPL (0x31), VRCPH (0x32), VRSQ (0x34), VRSQL (0x35), VRSQH (0x36).
  - VMOV (0x33) e VNOP (0x37).
  - Estado separado DIV_IN, DIV_OUT e indicador de entrada alta pendente.
  - Tabelas de aproximação RCP e RSQ de 512 entradas cada, integradas em `rsp_divide_tables.h`.
  - Tratamento de VRCPH/VRSQH para devolver a metade alta anterior e preparar a metade alta de entrada; VRCPL/VRSQL consomem a metade alta pendente ou usam entrada de 16 bits com sinal estendido quando não há entrada pendente.
  - Efeito colateral de carregar ACC_LO conforme o mapa de elementos para as operações escalares aplicáveis; VNOP não altera estado.
- Testes adicionados para VMOV/VNOP, VRCP, VRCPH, VRCPL e VRSQ, incluindo latch DIV_IN/DIV_OUT e o caso de entrada zero.
- CI da revisão corrigida: workflow `44901838371`; `native-host-tests` concluído com sucesso. `assemble-debug` ainda estava em andamento no momento deste registro. Não declarar build Android aprovada até o workflow terminar.
- Um workflow anterior (`44901810790`) falhou por chaves de inicialização ausentes nas tabelas; a correção foi registrada no commit `e75381a` e está sendo validada no workflow acima.
- Referências técnicas: SGI Nintendo 64 RSP Programmer's Guide, seções VU Divide Instructions; documentação RSP do r64emu (RCP_ROM/RSQ_ROM, DIV_IN/DIV_OUT); implementação de referência Project64 em `RSPInterpreterOps.cpp` e `RSPRegister.cpp`.


### Validação e revisão adicional da divisão RSP — 2026-11-21
- Workflow `44901838371` concluído com sucesso nos dois jobs: `native-host-tests` e `assemble-debug`. A compilação Android debug está aprovada para o commit validado por esse workflow.
- Revisão adicional da seleção de lane identificou que VRCP/VRCPL/VRSQL/VRSQ usavam apenas `element & 7`, o que falhava quando o campo de elemento está entre 0 e 7: nesses casos a lane de origem também depende da lane de destino. Corrigido para usar o mapa de seleção escalar `kElementMap[element][destinationLane]`, alinhando a origem à regra de single-lane do RSP.
- Teste de regressão atualizado: com `element=0`, destino na lane 3 e valor 1 na lane 3 de VT, VRCP agora retorna `0xC000` (resultado baixo da ROM RCP) em vez de ler incorretamente a lane 0.
- Commits dessa correção e teste registrados diretamente em `main`; workflow `44902293430` concluiu com sucesso em `native-host-tests` e `assemble-debug`, validando a correção e o teste.
- Ainda falta ampliar os vetores numéricos de VRCP/RSQ para entradas negativas, limites, valores de 32 bits e reset dos latches. O suporte permanece parcial até essa cobertura adicional e comparação independente bit a bit.


### Continuação do RSP — VABS (2026-11-21)
- Implementada a instrução vetorial VABS (função `0x13`). A operação usa o sinal de VS para copiar, zerar ou negar VT; o caso VT=`0x8000` com VS negativo satura VD em `0x7FFF`, enquanto ACC_LO preserva `0x8000`.
- VABS preserva VCO e atualiza ACC_LO lane a lane. Testes cobrem sinal positivo, negativo, zero, extremo `-32768`, preservação de VCO e leitura de ACC_LO via VSAR.
- Referências confrontadas: SGI Nintendo 64 RSP Programmer's Guide (descrição de VABS) e implementação interpretativa do Project64 (`RSPOp::Vector_VABS`).
- Commits de implementação e teste registrados em `main`; workflow `44902372095` concluiu com sucesso em `native-host-tests` e `assemble-debug`, validando VABS.


### Continuação do RSP — transferências vetoriais básicas de memória (2026-11-21)
- Implementado o primeiro subconjunto de `LWC2`/`SWC2`: LBV/SBV, LSV/SSV, LLV/SLV e LDV/SDV (subopcodes 0–3).
- Endereço efetivo usa offset de 7 bits com sinal, escalado pelo tamanho da transferência, e wrap de 4 KiB no DMEM.
- Loads preservam bytes não afetados e truncam no fim do registrador vetorial; stores fazem wrap da seleção de bytes do vetor, conforme a regra desses formatos.
- Teste de integração cobre os quatro tamanhos, offsets escalados e leitura/escrita de bytes no DMEM.
- Ainda não estão implementados LQV/LRV, LPV/LUV/LHV/LFV/LWV/LTV nem os stores correspondentes; este é apenas o primeiro grupo.
- CI pendente para implementação e testes antes da aprovação.


### Continuação do RSP — LQV/LRV/SQV/SRV (2026-11-21)
- Ampliado `LWC2`/`SWC2` com LQV/LRV/SQV/SRV (subopcodes 4 e 5), incluindo alinhamento de 16 bytes, seleção de elemento, truncamento dos loads e wrap dos stores.
- Testes seguem o padrão documentado de pares: LQV em endereço desalinhado seguido de LRV, e SQV seguido de SRV, verificando os bytes exatos transferidos e preservação das partes não afetadas do vetor.
- Subopcodes 0–5 agora cobrem LBV/LSV/LLV/LDV/LQV/LRV e SBV/SSV/SLV/SDV/SQV/SRV. Permanecem pendentes LPV/LUV/LHV/LFV/LWV/LTV e stores correspondentes.
- Workflows `44902543882` e `44902547799` concluíram com sucesso em `native-host-tests` e `assemble-debug`, validando LQV/LRV/SQV/SRV e a correção de wrap dos stores.


### Continuação do RSP — LPV/LUV (2026-11-21)
- Implementadas as cargas packed LPV (subopcode 6) e LUV (subopcode 7). Cada instrução lê oito bytes consecutivos de DMEM com offset escalado por 8 e grava um byte por lane nos oito elementos de 16 bits.
- LPV posiciona o byte em bits 15..8 (`byte << 8`); LUV posiciona em bits 14..7 (`byte << 7`). Os bits não usados são zerados.
- O elemento legal documentado é zero. A implementação segue esse formato arquitetural; stores SPV/SUV e demais formatos packed ainda não foram incluídos.
- Testes cobrem os oito lanes, deslocamento e limpeza dos bits não usados. Workflow `44902731964` concluiu `native-host-tests` com sucesso; `assemble-debug` permanece em andamento.


### Continuação do RSP — SPV/SUV (2026-11-21)
- Implementados SPV e SUV (subopcodes 6 e 7 de SWC2) para o formato legal documentado com element=0: oito bytes consecutivos são armazenados a partir das oito lanes do vetor.
- SPV extrai bits 15..8; SUV extrai bits 14..7. Testes verificam os oito bytes de cada formato e offset escalado por 8.
- O tratamento de elemento não zero permanece fora da implementação validada; o guia oficial indica element=0 para esses formatos packed. Ainda faltam LHV/SHV, LFV/SFV e LTV/STV.
- Workflow `44902752588` concluiu `native-host-tests` com sucesso; `assemble-debug` permanece em andamento.


### Continuação do RSP — LHV/SHV (2026-11-21)
- Implementados LHV e SHV (subopcode 8): oito bytes espaçados por dois bytes em DMEM, com offset escalado por 16.
- LHV coloca cada byte em bits 14..7 do elemento vetorial e zera os demais bits; SHV extrai os mesmos bits para gravar na memória.
- Testes verificam os oito elementos, stride de dois bytes, escalonamento do offset e transformação dos valores. Workflow `44902784631` concluiu `native-host-tests` com sucesso; `assemble-debug` permanece em andamento.
- A seleção por alinhamento e valores de elemento fora do padrão documentado ainda exige comparação mais ampla com testes de hardware/referência.


### Continuação do RSP — LFV (2026-11-21)
- Implementado LFV (subopcode 9 de LWC2), com offset escalado por 16 bytes.
- A instrução seleciona quatro bytes de uma janela DMEM de 16 bytes: element=0 usa posições 0,4,8,12; element=8 usa 8,12,0,4. Os bytes são convertidos para bits 14..7 de quatro lanes, preservando as lanes fora da metade selecionada.
- Somente element 0 e 8 são aceitos como valores documentados.
- A primeira versão tinha seleção incorreta para element=8; corrigida após comparação com a descrição detalhada de LFV. Testes agora cobrem os dois padrões.
- Commits iniciais: `a30566a` e `b7cb188`; correção: `d59858e`; testes corrigidos: `15480dd`. CI da correção pendente.
- SFV ainda não implementada.


### Continuação do RSP — SFV (2026-11-21)
- Implementado SFV (SWC2 subopcode 9), com offset escalado por 16.
- A operação monta a tabela temporária de bytes a partir das lanes `0,6,1,7,2,4,3,5`, deixando posições intermediárias zeradas; aplica o ajuste de element para valores 8..15 e grava quatro bytes espaçados por quatro posições, com wrap na janela de 16 bytes.
- Testes cobrem o exemplo element=5 (lanes 7,4,5,6), endereço desalinhado com wrap e um elemento que seleciona posições zeradas.
- Implementação: `961390a`; testes: `f03d589`. CI pendente.
- Observação: LFV foi corrigida no commit `d59858e` para usar os padrões distintos `0,4,8,12` e `8,12,0,4`; testes corrigidos em `15480dd`. A validação de CI dessa correção também precisa ser acompanhada.


### Estado da validação — LFV/SFV (2026-11-21)
- Correção LFV + testes atualizados: workflow `44903198432` — `native-host-tests` aprovado; `assemble-debug` ainda em execução na última verificação.
- Implementação SFV: workflow `44903225151` — `native-host-tests` aprovado; `assemble-debug` ainda em execução na última verificação.
- Testes SFV: workflow `44903235082` — `native-host-tests` aprovado; `assemble-debug` ainda em execução na última verificação.
- Portanto, LFV/SFV ainda não estão marcadas como integralmente validadas até a conclusão dos builds Android.

### Continuação do RSP — LTV/STV (2026-11-21)
- Implementados LTV (LWC2) e STV (SWC2), subopcode `0x0B`, com offset de 7 bits com sinal escalado por 16 bytes.
- A transferência movimenta oito halfwords de uma palavra DMEM alinhada de 128 bits. `vt & 0x18` seleciona o grupo de oito registradores; cada slice usa o registrador `(slice + (element >> 1)) & 7` dentro do grupo e a lane correspondente ao slice.
- Loads espalham os oito halfwords na diagonal do grupo; stores reúnem a mesma diagonal na palavra de memória. O bit menos significativo de element não participa do índice, conforme a fórmula do manual oficial.
- Teste de integração cobre os quatro grupos de registradores, as oito diagonais, ida e volta LTV/STV, offset escalado e a última palavra alinhada do DMEM.
- Implementação: commit `25a87d1`; testes: `3d46396`; correção do escalonamento do offset: `fecdc75`.
- CI dos commits LTV/STV: workflows `44903672783` (testes) e `44903680009` (escala do offset), aguardando conclusão na última verificação.
- Limitação atual: o RSP ainda não modela os três slots de atraso de loads vetoriais nem o interlock de pipeline descritos no manual; isso afeta o timing de todas as cargas vetoriais, não apenas LTV. O comportamento para endereços base não alinhados e elementos ilegais ainda precisa de validação específica.
- Referência normativa: SGI Nintendo 64 RSP Programmer's Guide, seção Transpose (p. 54) e entrada LTV (p. 192); a codificação `0x0B` também é corroborada pelo código do Mupen64Plus. A tabela `0x08` encontrada em uma referência secundária foi tratada como erro documental.

### Atualização de CI — LFV/SFV (2026-11-21)
- A verificação final confirmou que os quatro workflows `44903198432`, `44903212457`, `44903225151` e `44903235082` concluíram com sucesso nos dois jobs: `native-host-tests` e `assemble-debug`. O status pendente descrito na seção anterior foi superado.
### Revisão de precisão — LTV/STV, SWV e LWV (2026-11-21)
- A primeira implementação de LTV/STV foi refinada após confronto com o código do Ares e a descrição de transposição do guia SGI. A versão atual aplica a rotação dos halfwords e respeita a janela de 16 bytes alinhada em 8 bytes, incluindo a seleção influenciada pelo bit 3 do endereço no LTV.
- LTV usa a janela iniciada em `windowBase + ((element + (address & 8)) & 15)`; cada halfword é distribuído para `vtBase + ((slice + (element >> 1)) & 7)`, lane `slice`. STV reúne a mesma diagonal e inicia a gravação em `windowBase + ((address & 7) - (element & 0x0E))`, com wrap de 16 bytes.
- Implementado SWV (SWC2 subopcode `0x0A`): grava 16 bytes, começando na posição de elemento indicada, com rotação de origem e wrap dentro de uma janela de 16 bytes ancorada no limite de 8 bytes.
- LWV (LWC2 subopcode `0x0A`) foi modelado como no-op, pois não está presente no RSP de varejo do Nintendo 64.
- Testes atualizados verificam as oito diagonais e quatro grupos de registradores, a rotação de origem/destino de LTV/STV, a janela final do DMEM, SWV com elemento 13 e base desalinhada em 8 bytes, e o no-op de LWV.
- Commits de refinamento LTV/STV: `9a9d4f2` e `a5a1978`; SWV/LWV: `f5c9b3a`, testes `3e9a4d0`, correção do decoder `16ec857`.
- Workflow mais recente `44903884566`: `native-host-tests` aprovado; `assemble-debug` ainda em execução na última verificação.
- Limitação remanescente: o pipeline do RSP ainda não implementa os três slots de atraso/interlocks das cargas vetoriais. Elementos ilegais e alinhamentos fora das condições documentadas ainda exigem validação adicional.
### Revisão de precisão — janelas DMEM de LHV/SHV/LFV/SFV (2026-11-21)
- A comparação com o interpretador do Ares revelou que esses formatos usam janelas de 16 bytes ancoradas em um limite de 8 bytes, e não simplesmente endereços lineares a partir do endereço efetivo.
- LHV agora calcula o início da sequência a partir de `(address & 7) - element` e percorre os oito bytes com stride 2 e wrap de 16 bytes. SHV usa a janela equivalente, seleciona bytes a partir do elemento vetorial e recompõe o byte de saída com os bits 14..7.
- LFV agora soma o desalinhamento de 8 bytes aos padrões `0,4,8,12` ou `8,12,0,4` e faz wrap dentro da janela. SFV agora grava os quatro bytes a cada quatro posições dentro da mesma janela de 16 bytes, inclusive quando o bit 3 do endereço está ligado.
- Testes foram ampliados para endereços com deslocamento de 6 bytes dentro da janela; LHV/SHV atravessam o limite de 16 bytes, LFV verifica as duas metades com base desalinhada, e SFV verifica o wrap com bit 3 do endereço ligado.
- Implementação: commit `c51d32a`; testes de LHV/SHV/LFV: `13967fd`; teste adicional de SFV: `b707097`.
- Workflow `44904063267`: `native-host-tests` aprovado; `assemble-debug` ainda em execução na última verificação.

### Continuação do RSP — transferências COP0/SP (2026-11-22)
- Implementados MFC0 e MTC0 do COP0 do RSP para os oito registradores SP já expostos pelo barramento: SP_MEM_ADDR, SP_DRAM_ADDR, SP_RD_LEN, SP_WR_LEN, SP_STATUS, SP_DMA_FULL, SP_DMA_BUSY e SP_SEMAPHORE.
- A leitura/escrita usa os acessos de 32 bits do barramento, preservando os efeitos colaterais existentes (por exemplo, leitura de SP_SEMAPHORE e comandos de SP_STATUS, além do início síncrono de DMA ao escrever SP_RD_LEN/SP_WR_LEN).
- Codificações COP0 não suportadas ou índices sem registrador mapeado continuam gerando falha explícita, em vez de retornar valores inventados.
- Teste cobre MTC0/MFC0 de SP_MEM_ADDR, limpar HALT por SP_STATUS e semântica de leitura do SP_SEMAPHORE.
- Implementação e testes aguardam CI.
- Limitação: registradores DPC COP0 (índices 8–15) ainda não estão mapeados no barramento; não fazem parte desta etapa.


### Continuação do RSP — shifts escalares e JALR (2026-11-22)
- Adicionados SRA, SLLV, SRLV e SRAV ao decodificador SPECIAL do Scalar Unit.
- Adicionado JALR com link em `rd`, alvo alinhado ao PC de 12 bits do RSP e execução do delay slot.
- Os testes verificam deslocamentos aritméticos/lógicos, deslocamento variável e link/delay slot do JALR.
- Implementação e testes aguardam CI.
- Situação atualizada após commits seguintes: ADD/ADDI/SUB, SLT/SLTU/SLTI/SLTIU, shifts escalares e branches com link foram implementados. Permanecem pendentes revisão aprofundada de semântica COP0/DPC, interlocks e instruções de multiplicação/divisão escalar (não disponíveis no RSP padrão).


### Correção de CI — testes COP0 (2026-11-22)
- Os primeiros workflows da adição COP0 falharam por um caractere literal de escape de nova linha inserido indevidamente na chamada do teste em main; o código de produção compilou.
- A chamada foi corrigida para uma quebra de linha real. A revisão posterior de shifts/JALR também está incluída nos workflows mais recentes.
- A validação final deve considerar os workflows posteriores a esta correção, não os runs iniciais com erro de formatação.


### Continuação do RSP — aritmética e comparações escalares (2026-11-22)
- Adicionados ADD, ADDI, SUB, SLT, SLTU, SLTI e SLTIU ao subconjunto escalar do RSP.
- ADD/ADDI e SUB usam aritmética de 32 bits com wrap, conforme a unidade escalar sem exceções aritméticas do RSP; comparações signed e unsigned são separadas.
- Testes cobrem valores negativos, limites de 32 bits e comparações signed/unsigned.
- CI pendente.


### Continuação do RSP — branches com link (2026-11-22)
- Implementados BLTZAL e BGEZAL (REGIMM rt=16 e rt=17).
- O registrador RA (r31) recebe PC+8 mesmo quando a condição não é satisfeita, de acordo com o comportamento documentado para o RSP.
- Ambos os caminhos preservam o delay slot; testes cobrem BLTZAL tomado e BGEZAL não tomado, incluindo link e fluxo de execução.
- Referências consultadas: manual de COP0/RSP da SGI, implementação Mupen64Plus RSP e documentação RustyN64.
- CI pendente.


## 14. DPC / COP0 do RSP — primeira camada implementada em 2026-11-22

### Implementado nesta etapa
- Janela MMIO DPC mapeada em `0x04100000–0x0410001F`, separada das regiões SP e MI.
- Registradores DPC_START (c8), DPC_END (c9), DPC_CURRENT (c10), DPC_STATUS (c11), DPC_CLOCK (c12), DPC_BUFBUSY (c13), DPC_PIPEBUSY (c14) e DPC_TMEM (c15).
- MFC0/MTC0 do RSP agora direcionam c0–c7 para os registradores SP e c8–c15 para os registradores DPC.
- DPC_START armazena endereço alinhado em 8 bytes e inicializa DPC_CURRENT.
- DPC_END armazena o limite alinhado da lista de comandos.
- DPC_STATUS implementa os comandos de limpar/definir XBUS_DMEM_DMA e FREEZE, limpar FLUSH e zerar contadores/busy correspondentes.
- Registradores de leitura como CURRENT e CLOCK ignoram escritas.
- Testes nativos adicionados para o mapeamento, alinhamento, comandos de status e transferências MFC0/MTC0 do RSP.

### Limitações importantes
- **RDP ainda não executa comandos.** Escrever DPC_END apenas registra o endereço final; não há consumo de command buffer, rasterização, pipeline de triângulos, textura ou framebuffer.
- DPC_CURRENT não avança por consumo de comandos; DPC_CLOCK/BUFBUSY/PIPEBUSY/TMEM permanecem zerados até existir o motor RDP e sua temporização.
- A semântica implementada é uma base de registradores para integração futura, não uma simulação completa do DP. Interrupções DP, XBUS DMA real, freeze/flush em pipeline e estados busy ainda precisam ser ligados ao RDP.

### Próxima etapa recomendada
Implementar uma fila/ponte de submissão de comandos entre DPC e RDP, com leitura de palavras da RDRAM ou SP DMEM conforme XBUS_DMEM_DMA, avanço de DPC_CURRENT e testes de consumo sem rasterização. Depois disso, iniciar o decodificador de comandos RDP e o pipeline de framebuffer.



### 14.1. Leitura da lista de comandos DPC — implementação inicial

- A interface do barramento agora oferece `readDpCommand(uint64_t&)` para o futuro RDP obter uma palavra de comando de 64 bits por chamada.
- Em modo normal, a palavra é lida da RDRAM no endereço indicado por DPC_CURRENT.
- Com XBUS_DMEM_DMA ativo, a leitura vem da SP DMEM, usando o endereço de 12 bits alinhado dentro da DMEM.
- Cada leitura bem-sucedida avança DPC_CURRENT em oito bytes; DPC_END é tratado como limite exclusivo.
- `dpCommandsRemaining()` informa quantas palavras completas de 64 bits ainda podem ser lidas.
- FREEZE impede a leitura e preserva o cursor; ao remover FREEZE, a leitura pode continuar.
- Testes cobrem ordem big-endian, leitura em RDRAM, leitura em DMEM, avanço do cursor, fim da lista e freeze/unfreeze.

**Limites desta etapa:** o RDP ainda não chama essa interface porque seu módulo não existe. Não há decodificação de opcodes, rasterização, atualização de DPC_CLOCK/busy em tempo de hardware, interrupção DP ou processamento real de FLUSH. A interface prepara a fronteira entre barramento e RDP, sem afirmar execução gráfica.


## 15. RSP — operações vetoriais de quantização e arredondamento

Implementadas em 2026-11-22:
- **VMULQ (funct 0x03):** multiplica dois operandos assinados, aplica o ajuste de arredondamento de produtos negativos, grava o produto no acumulador com deslocamento de 16 bits e produz resultado saturado com os quatro bits inferiores zerados.
- **VMACQ (funct 0x0B):** ajusta o acumulador de 48 bits em direção à fronteira de quantização de `0x20_0000`, satura o resultado deslocado e zera os quatro bits inferiores da saída.
- **VRNDP (funct 0x02) e VRNDN (funct 0x0A):** usam o sinal do acumulador para decidir se somam o elemento de VT; o bit menos significativo do campo VS seleciona deslocamento de 16 bits. O acumulador é mantido com sinal em 48 bits e o resultado é saturado a partir da parte média.
- Os elementos de VT são capturados antes da escrita em VD para evitar corrupção quando os registradores de origem e destino coincidem.
- Testes verificam resultados e fatias do acumulador após VMULQ, VRNDP, VRNDN e VMACQ.

**Referência técnica:** SGI Nintendo 64 RSP Programmer's Guide, seção Vector Unit Instructions, e implementação/documentação pública do projeto RustyN64 usada como comparação independente. A semântica deve continuar sendo confrontada com testes de sistema independentes; aprovação dos testes locais não prova compatibilidade completa de microcódigo.


### 15.1. Família de opcodes VZERO

- Implementada a família de 19 codificações documentadas pelos testes de sistema como comportamento VZERO: `0x12`, `0x16–0x1C`, `0x1E–0x1F`, `0x2E–0x2F` e `0x38–0x3E`.
- Cada codificação grava zero em todas as lanes de VD e grava `VS + VT[element]` em ACC_LO, com soma de 16 bits.
- As fontes são capturadas antes da escrita para preservar comportamento quando VD coincide com VS ou VT.
- Teste percorre as 19 codificações e verifica VD e ACC_LO em todas as oito lanes.
- Esta classificação vem de observação de testes de sistema/documentação independente; são codificações reservadas/não documentadas como instruções usuais, não novos mnemonics confirmados pelo manual oficial.


## 16. Primeiro decodificador de comandos do RDP (2026-11-22)

### Implementado nesta etapa
- Criados `n64/rdp.h` e `n64/rdp.cpp`, com uma interface isolada `RdpCommandDecoder`.
- Extração do opcode de 6 bits (bits 61:56) do cabeçalho de 64 bits.
- Enquadramento por comprimento:
  - comandos comuns e reservados: 1 palavra de 64 bits;
  - Texture Rectangle (0x24) e Texture Rectangle Flip (0x25): 2 palavras;
  - Fill Triangle (0x08–0x0F): 4 palavras-base, mais 8 palavras de shade, 8 de texture e 2 de Z conforme os três bits inferiores do opcode.
- Criado `tests/rdp_tests.cpp` cobrindo extração do opcode, comprimentos de comandos de uma palavra, os oito formatos de triângulo e os dois retângulos de textura.
- O decoder foi incluído nos CMake Android e de testes nativos.

### Limites explícitos
- Esta etapa só identifica opcode e comprimento. **Não executa comandos, não rasteriza triângulos/retângulos e não escreve framebuffer**.
- Ainda não existe integração do decoder ao consumo do DPC. É necessário implementar o consumo atômico de comandos completos, sem avançar DPC_CURRENT quando o buffer contém apenas parte de um comando multi-palavra.
- O sinalizador `knownOpcode` identifica as faixas de comandos documentadas (0x08–0x0F e 0x24–0x3F); as faixas restantes são enquadradas como cabeçalhos de uma palavra, sem execução.
- Referência técnica independente consultada: `doublegate/RustyN64/docs/rdp.md`, seção “The command decoder (T-31-001)”, que atribui os comprimentos às regras do N64brew Reality Display Processor/Commands. A implementação atual usa essas regras de framing, não a implementação de rasterização do projeto de referência.

### Próxima etapa
- Criar testes de integração DPC/RDP para garantir que um comando multi-palavra só seja retirado do fluxo quando todas as palavras estiverem disponíveis.
- Depois implementar a dispatch de comandos de sincronização e estado, mantendo renderização marcada como ausente até existir pipeline de pixels validado.


### 16.1. Consumo atômico de comandos DPC/RDP (2026-11-22)

- O barramento agora oferece `peekDpCommand()` para consultar o cabeçalho sem avançar DPC_CURRENT.
- `readDpCommandWords()` consome uma quantidade completa de palavras de 64 bits em uma única operação lógica. Verifica primeiro se todo o comando está disponível e, em modo RDRAM, se todo o intervalo cabe na memória.
- Se o comando estiver incompleto, congelado, desalinhado ou fora dos limites, a função falha sem avançar DPC_CURRENT e sem modificar o buffer de saída.
- No modo XBUS, as leituras usam a janela circular de 4 KiB da SP DMEM.
- `readDpCommand()` foi mantido como conveniência para consumir uma única palavra e agora usa o caminho atômico.
- Teste de regressão: Texture Rectangle de duas palavras com apenas a primeira palavra publicada; a leitura é recusada e o cursor permanece intacto. Após publicar a segunda palavra, ambas são consumidas e DPC_CURRENT avança oito bytes por palavra.
- **Limite:** o RDP ainda precisa chamar o decoder, determinar o comprimento e solicitar o bloco completo. Esta alteração fornece a primitiva transacional, mas não implementa dispatch, rasterização ou framebuffer.


### 16.2. Ponte inicial entre DPC e decoder RDP (2026-11-22)

- Criada a estrutura `RdpCommandPacket`, que armazena metadados e até 22 palavras de 64 bits — limite necessário para os comandos de triângulo com todos os blocos opcionais.
- Criada `RdpCommandStream::fetchNext()`: consulta o cabeçalho com `peekDpCommand()`, usa `RdpCommandDecoder` para obter o comprimento e solicita ao barramento a retirada atômica do comando completo.
- Se o comando estiver incompleto ou o DPC estiver congelado, a função retorna falso sem alterar o pacote do chamador nem DPC_CURRENT.
- Adicionado teste de integração: Texture Rectangle incompleto é retido; quando a segunda palavra é publicada, o pacote recebe opcode, tamanho e as duas palavras, e o cursor avança pelo bloco completo.
- O alvo de testes RDP agora liga também `bus.cpp` e `rdram.cpp` para testar o caminho integrado.
- **Limite:** esta ponte apenas transporta/enquadra comandos. Nenhum opcode é executado; o pipeline de rasterização e framebuffer continua ausente.


### 16.3. Primeiro processamento de comandos RDP: sincronização (2026-11-22)

- Criado `RdpCommandProcessor::processNext()`, que obtém o próximo comando completo pelo fluxo DPC/RDP e despacha comandos com implementação explícita.
- Implementados os comandos de sincronização:
  - `0x26 SYNC_LOAD`: registra a ocorrência do marcador de sincronização de carga.
  - `0x27 SYNC_PIPE`: registra a ocorrência do marcador de sincronização do pipeline.
  - `0x28 SYNC_TILE`: registra a ocorrência do marcador de sincronização de tile.
  - `0x29 SYNC_FULL`: registra a ocorrência e afirma a interrupção DP no MI (fonte 5).
- Estado de diagnóstico `RdpSyncState` mantém contadores para os quatro comandos.
- Comandos ainda não implementados são consumidos segundo o enquadramento e retornam `Unsupported`; não são tratados como renderizados.
- Testes verificam os quatro comandos, a interrupção DP somente após SYNC_FULL, fim do fluxo e retorno explícito para comando não implementado.
- **Limite importante:** sincronização é sequencial/simplificada. Não existe pipeline assíncrono de pixels para drenar, então SYNC_PIPE/SYNC_TILE/SYNC_LOAD são marcadores sem espera de hardware. SYNC_FULL afirma a interrupção DP imediatamente após o processamento sequencial; não há rasterização, textura, color combiner ou framebuffer.


### 16.4. Captura inicial de comandos de estado RDP (2026-11-22)

- O processador agora reconhece e retém o comando bruto de `SET_SCISSOR (0x2D)`, `SET_OTHER_MODES (0x2F)`, `SET_TILE_SIZE (0x32)`, `SET_TILE (0x35)`, `SET_FILL_COLOR (0x37)`, `SET_TEXTURE_IMAGE (0x3D)` e `SET_COLOR_IMAGE (0x3F)`.
- Os comandos de tile são armazenados em oito slots; o índice é extraído dos bits 26:24 do cabeçalho.
- Flags indicam quais estados já foram configurados; isso evita confundir um comando ainda não recebido com um valor zero.
- Teste envia os sete comandos pelo DPC e verifica que os valores brutos e os slots de tile foram preservados.
- **Limite:** esta é captura bruta de estado, não interpretação completa dos campos nem configuração de um rasterizador. Não há leitura de textura, cálculo de coordenadas, color combiner, depth buffer ou escrita de pixels.


### 16.5. Decodificação tipada do estado RDP (2026-11-22)

- O estado bruto capturado na etapa 16.4 agora também é convertido em estruturas tipadas, sem ainda executar rasterização.
- **SET_SCISSOR (0x2D):** extrai XH, YH, XL e YL como coordenadas inteiras em formato 10.2, além dos bits de habilitação de campo e seleção de linha ímpar.
- **SET_TILE (0x35):** extrai formato, tamanho, stride em palavras de 64 bits, base TMEM, paleta e os parâmetros S/T de clamp, mirror, mask e shift. Cada um dos oito descritores mantém seu próprio estado.
- **SET_TILE_SIZE (0x32):** extrai SL, TL, SH e TH em formato 10.2 para o tile selecionado.
- **SET_TEXTURE_IMAGE (0x3D) e SET_COLOR_IMAGE (0x3F):** extraem formato, tamanho, largura (campo codificado + 1) e endereço DRAM de 26 bits.
- Os comandos brutos continuam preservados para diagnóstico e comparação futura.
- Teste dedicado constrói comandos com campos não triviais e verifica cada campo extraído, inclusive limites/flags e seleção de tile.
- Fontes de referência: resumo oficial SGI/Nintendo Ultra64 RDP Command Summary (1996), tabelas Set Color Image, Set Texture Image, Set Tile, Set Tile Size e Set Scissor; documentação técnica RustyN64 consultada como comparação independente.
- **Limite:** não há validação de formatos legais, leitura de textura, carga de TMEM, cálculo de coordenadas, color combiner, blender, depth/coverage ou escrita de pixels. A decodificação de SET_OTHER_MODES permanece apenas bruta.
- Commits desta etapa: estruturas tipadas `d3bdc94`, decodificação `5359c00`, testes `7112f06`.

### 17. Validação de CI e artefato Android — 2026-11-22

- Workflow `45019342842`, commit `93f3c47204cfd2dc7c7f0c6df92b92a24003c559`: `native-host-tests` e `assemble-debug` concluíram com sucesso; a etapa `Upload debug APK` também foi concluída.
- Artefato `m64plus-debug-apk`, ID `14752977861`, tamanho 558.571 bytes, SHA-256 `efa160d9a2393299013a0400cdc358993b58b5617a6024531cf196efc43d8cbb`, expira em 2027-02-20.
- Esse APK corresponde ao commit acima, que inclui a captura bruta de estado RDP da seção 16.4. **Não corresponde ainda às alterações de decodificação tipada da seção 16.5**, que foram feitas depois.
- A validação do commit mais recente precisa ser feita no próximo workflow gerado após os commits 16.5.


### 16.6. Decodificação de SET_OTHER_MODES e SET_COMBINE (2026-11-22)

- **SET_OTHER_MODES (0x2F):** agora interpreta ciclo (1-cycle, 2-cycle, copy, fill), perspectiva, detail/sharpen, LOD, TLUT e tipo, sample type, mid-texel, bilerp por ciclo, convert-one, keying, RGB/alpha dither.
- Também extrai os oito seletores P/A/M/B do blender (quatro por ciclo), force-blend, alpha-coverage-select, coverage-times-alpha, Z mode, coverage destination, color-on-coverage, image-read, Z update/compare, antialias, Z source e alpha compare.
- **SET_COMBINE (0x3C):** decodifica os 16 seletores do color combiner: A/B/C/D para RGB e A/B/C/D para alpha, em cada um dos dois ciclos.
- Os comandos brutos permanecem armazenados junto com as estruturas tipadas.
- Teste dedicado constrói palavras com seletores distintos e verifica os campos dos dois ciclos, os controles do blender e os modos de textura/depth/coverage.
- Layouts confrontados com o documento oficial SGI/Nintendo Ultra64 RDP Command Summary (versão 2.0, 1996), tabelas 19 (Set Combine Mode) e 20 (Set Other Modes). A descrição do guia profissional SGI/Nintendo sobre o blender foi usada como contexto adicional.
- **Limite:** decodificar muxes não executa a equação do color combiner nem o blender por pixel. Não há rasterização, textura/TMEM, coverage, Z-buffer ou framebuffer nesta etapa.
- Commits: estruturas `f83ce85`, decodificação `c24cf6c`, testes `ca559a4`, ajuste do valor esperado no teste `70aebf4`.


### 16.7. Estado de cores e Z image do RDP (2026-11-22)

- O processador agora interpreta os comandos de registradores de cor:
  - `SET_FILL_COLOR (0x37)`: palavra de cor empacotada de 32 bits.
  - `SET_FOG_COLOR (0x38)`, `SET_BLEND_COLOR (0x39)` e `SET_ENV_COLOR (0x3B)`: RGBA de 8 bits por canal, preservado como palavra de 32 bits.
  - `SET_PRIM_COLOR (0x3A)`: cor RGBA, Prim LOD Minimum (5 bits) e Prim LOD Fraction (8 bits).
- **SET_Z_IMAGE (0x3E):** endereço base DRAM de 26 bits.
- Todos os comandos brutos continuam disponíveis para diagnóstico.
- Teste integrado publica os seis comandos pela lista DPC e verifica cada campo e flag.
- Layout confrontado com o SGI/Nintendo Ultra64 RDP Command Summary 2.0 (1996), tabelas Set Fill Color, Set Fog Color, Set Blend Color, Set Prim Color, Set Env Color e Set Z Image.
- **Limite:** os registradores são armazenados; não existe ainda operação de fill, mistura de cores, teste de profundidade ou escrita em Z/color framebuffer. A etapa não implementa pixels.
- Commits: estruturas `3618d37`, decodificação `131d1f6`, testes `308952c`.


### 16.8. Conversão YUV e controles de color key do RDP (2026-11-22)

- Implementado o decodificador de `SET_CONVERT (0x2C)`, armazenando os seis coeficientes K0–K5 como valores assinados de 9 bits, com extensão de sinal na faixa de -256 a +255.
- Implementado `SET_KEY_R (0x2B)`, com width de 12 bits, center e scale de 8 bits.
- Implementado `SET_KEY_GB (0x2A)`, com width G/B de 12 bits e center/scale de 8 bits para cada canal.
- Os três comandos mantêm a palavra bruta original e flags próprias de presença, além dos campos tipados.
- Teste integrado publica os comandos via DPC e verifica os campos, incluindo coeficientes negativos e extremos assinados.
- **Limite:** os valores são apenas decodificados e armazenados. Não foi implementada conversão YUV→RGB, keying por pixel, comparação de limiar ou rasterização; nenhum pixel é produzido nesta etapa.

- Ajuste de regressão após ampliar o dispatch: o teste genérico de comando não suportado usava `0x2A`, que passou a ser corretamente reconhecido como `SET_KEY_GB`. O cenário de comando ainda não implementado foi transferido para `0x30` (`LOAD_TLUT`), mantendo a verificação da resposta `Unsupported`.
