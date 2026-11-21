# Auditoria técnica — M64plus

**Repositório:** `jovemaprendz55-byte/M64plus`  
**Branch de registro:** `main`  
**Escopo:** auditoria estática e revisão das implementações já examinadas.  
**Modo:** auditoria contínua com correções registradas diretamente na `main`.  
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
