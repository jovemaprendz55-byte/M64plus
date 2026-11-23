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


### 16.9. Registrador de profundidade primitiva do RDP (2026-11-22)

- Implementado `SET_PRIM_DEPTH (0x2E)` com extração do registrador Primitive Z nos bits 31–16 e Primitive Delta Z nos bits 15–0, preservando ambos como valores de 16 bits.
- A palavra bruta e uma flag de presença são mantidas para diagnóstico.
- Teste integrado verifica os dois campos com valores não triviais e a preservação do comando original.
- Layout conferido no SGI/Nintendo Ultra64 RDP Command Summary, Tabela 26; a referência da API `gDPSetPrimDepth` descreve os formatos e uso dos valores Z/delta-Z.
- **Limite:** ainda não há teste de profundidade por pixel, conversão para o formato do Z-buffer, atualização do Z-buffer ou rasterização.


### 16.10. Base de memória de textura TMEM (2026-11-22)

- Criada a estrutura `RdpTextureMemory` com 4.096 bytes (4 KiB), correspondente à memória on-chip de texturas do RDP.
- Implementadas operações de byte e palavra de 64 bits, com representação big-endian, endereçamento de 12 bits e wrap no limite de 4 KiB.
- Adicionado reset que limpa toda a memória; a estrutura foi integrada ao estado do processador RDP.
- Testes cobrem memória inicial zerada, leitura/escrita de byte, round-trip de 64 bits, cruzamento do fim da TMEM, wrap de endereço e reset.
- **Limite importante:** a TMEM está pronta como armazenamento básico, mas ainda não recebe dados dos comandos `LOAD_TILE`, `LOAD_BLOCK` ou `LOAD_TLUT`. Swizzle de bancos/halfwords, expansão TLUT e regras de transferência por formato ainda não foram implementados. Nenhuma textura pode ser amostrada ou renderizada nesta etapa.
- Referência arquitetural: Nintendo 64 Programming Manual, capítulo 13 (Texture Mapping), descreve TMEM on-chip de 4 KiB, tiles e caminhos de carga.



### 16.11. Decodificação inicial dos comandos de carga de textura (2026-11-22)

- O processador reconhece os cabeçalhos `LOAD_TLUT (0x30)`, `LOAD_BLOCK (0x33)` e `LOAD_TILE (0x34)`.
- Criado estado tipado comum com tipo de carga, índice de tile, SL/TL/SH e TH ou DXT, além da palavra bruta.
- Testes integrados pelo fluxo DPC verificam os três tipos e os campos do comando `LOAD_TILE`.
- O teste de comando reservado foi atualizado para opcode `0x31`, que permanece não implementado.
- **Limite:** esta etapa somente interpreta os cabeçalhos. Não lê texels da RDRAM, não escreve na TMEM, não calcula stride/padding, não executa DXT/interleaving e não expande TLUT. A TMEM continua sem ser alimentada e não há amostragem ou renderização de texturas.
- Referências: SGI/Nintendo RDP Command Summary (campos de Load TLUT) e Nintendo 64 Programming Manual, capítulo 13, seções de Tile Loading e Texture Loading.


## 16.12 LOAD_TILE — primeira transferência de texels para TMEM (2026-11-22)

**Implementado nesta etapa:** o comando LOAD_TILE (0x34) agora realiza uma primeira transferência lógica quando SET_TEXTURE_IMAGE e SET_TILE estão configurados:
- interpreta SL/TL/SH/TH como coordenadas 10.2 e usa as coordenadas inteiras;
- calcula a origem da região em RDRAM com base na largura e no tamanho do texel da imagem;
- copia texels de 4, 8, 16 ou 32 bits para TMEM;
- respeita LINE do descritor SET_TILE como stride em unidades de 64 bits;
- mantém ordem big-endian e empacotamento de nibbles para texels de 4 bits;
- valida toda a origem antes de modificar TMEM;
- adiciona testes DPC para sub-retângulo 8-bit em múltiplas linhas e dados 4-bit empacotados.

**Limitações / investigação pendente:**
- transferência linear lógica; ainda não reproduz a organização física real de TMEM, swizzle de bancos/halfwords em linhas ímpares ou regras específicas de formato;
- coordenadas fracionárias são truncadas para texel inteiro;
- LOAD_BLOCK e LOAD_TLUT continuam sem transferência de dados;
- não há amostragem de textura nem rasterização;
- ausência de SET_TEXTURE_IMAGE/SET_TILE ou leitura inválida retorna Unsupported depois que o comando foi consumido.

**Validação:** testes nativos e compilação Android desta alteração dependem do workflow do commit; não declarar aprovados até o GitHub Actions concluir.


## 16.13 LOAD_TLUT — expansão de entradas de paleta (2026-11-22)

Foi implementada a transferência inicial do comando LOAD_TLUT (0x30):
- exige SET_TEXTURE_IMAGE em 16 bits e um descritor SET_TILE válido;
- calcula a quantidade como (SH >> 2) + 1, conforme o campo count usado pelas macros GBI;
- exige endereço TMEM na metade alta (palavras 256 a 511) e limita a 256 entradas;
- lê cada cor de 16 bits da RDRAM em big-endian e replica quatro vezes para preencher uma palavra TMEM de 64 bits;
- valida todas as leituras de origem antes de alterar TMEM;
- atualiza o tamanho do tile associado com os campos do comando de carga;
- adiciona teste com três entradas de paleta e verificação das três palavras de 64 bits.

**Limitações:** não há ainda interpretação de formato RGBA16 versus IA16 durante a amostragem, nem leitura de índices CI pelo sampler (ainda inexistente). A implementação não simula temporização/pipeline do carregamento. O caminho LOAD_BLOCK continua apenas decodificado, sem transferência de texels.

Referências técnicas consultadas: Nintendo 64 Programming Manual, capítulo 13.9 (LOAD_TLUT expande cada cor de 16 bits para uma palavra de 64 bits na metade alta da TMEM); SGI/Nintendo Ultra64 RDP Command Summary, tabela LOAD_TLUT (SL/SH, count e exigência de endereço TMEM na metade alta); macros GBI gDPLoadTLUTCmd (count codificado em bits 23:14).


## 16.14 LOAD_BLOCK — primeira transferência linear para TMEM (2026-11-22)

Foi implementado um primeiro caminho de transferência para LOAD_BLOCK (0x33):
- exige SET_TEXTURE_IMAGE e SET_TILE previamente configurados;
- calcula o número de texels por ((SH - SL) >> 2) + 1 e rejeita coordenadas fora da ordem ou não alinhadas em 10.2;
- usa TL e SL como posição inicial na imagem de origem;
- transfere sequencialmente texels de 4, 8, 16 ou 32 bits para TMEM a partir do endereço TMEM do tile;
- preserva ordem big-endian e o empacotamento de nibbles para 4 bits;
- limita o tamanho da operação a 2.048 texels e valida a origem inteira antes de escrever;
- adiciona testes para fluxo de oito texels de 8 bits, retenção do campo DXT e fluxo empacotado de 4 bits.

**Limitações:** esta etapa é deliberadamente uma transferência linear lógica. O valor DXT é decodificado e preservado, mas ainda não controla o avanço de linha nem o interleave/swizzle das palavras de TMEM. Também faltam o tratamento de formato por bancos, casos de fronteira da TMEM, e comparação com um oracle de hardware/emulador de referência. Portanto, não considerar LOAD_BLOCK cycle-accurate ou fisicamente fiel ainda. Nenhuma amostragem de textura ou rasterização foi adicionada.


## 16.15 LOAD_BLOCK — acumulador DXT e troca das linhas ímpares (2026-11-22)

O caminho LOAD_BLOCK foi ampliado para aplicar o comportamento de DXT documentado:
- os texels são preparados em um buffer temporário de transferência, preservando o empacotamento 4-bit e a ordem big-endian;
- a cada palavra completa de 64 bits, o acumulador fracionário recebe DXT;
- cada carry em 0x800 incrementa o contador de linha;
- palavras pertencentes a linhas ímpares têm suas duas metades de 32 bits trocadas;
- a transferência total é limitada a 4 KiB de TMEM, além do limite de 2.048 texels;
- LOAD_BLOCK atualiza SL/TL/SH no descritor do tile, conforme o comportamento dos comandos de carga;
- teste específico usa DXT=0x400 para comprovar que a terceira palavra (primeira da linha ímpar) troca os dois grupos de quatro bytes.

**Limitações:** o algoritmo ainda é um modelo funcional simplificado para palavras completas de 64 bits. O último fragmento menor que oito bytes não recebe troca de metades; não há validação por comparação binária com hardware real ou com um RDP de referência. A organização por bancos de TMEM e os formatos YUV/RGBA ainda precisam de tratamento específico. Não há sampler nem rasterizador.

**Referências:** Nintendo 64 Programming Manual, seção 13.9.3 (Load Block), descreve o acumulador DXT, incremento de linha e troca das linhas ímpares; RDP Command Summary também documenta o uso de DXT para interleaving.


## 16.16 LOAD_BLOCK — restrição explícita dos tamanhos de texel (2026-11-22)

- A revisão comparativa do caminho de carga mostrou que tratar 4-bit e 32-bit como cópia linear genérica pode produzir TMEM incorreta.
- O caminho comum LOAD_BLOCK foi restringido a fontes de 8 e 16 bits, que são os formatos cobertos pelo caminho linear implementado nesta etapa.
- Cargas de 4 bits agora retornam `Unsupported`; a documentação de referência RustyN64 registra que 4-bit LOAD_BLOCK não é um caminho válido de hardware e que jogos normalmente carregam os bytes como 8-bit para depois amostrar nibbles no tile de renderização.
- Cargas de 32 bits também retornam `Unsupported` até existir o caminho dedicado que separa os componentes entre as metades/bancos da TMEM. O código Project64 mantém uma rotina específica `LoadBlock32b`, evidência de que a cópia linear atual não era suficiente.
- Testes agora cobrem transferência 16-bit big-endian e rejeição explícita dos tamanhos 4-bit e 32-bit, além dos testes anteriores de 8-bit e DXT.
- Referências comparativas: RustyN64 RDP documentation (residual R-7) e Project64 `Source/Project64-video/rdp.cpp` (`LoadBlock32b`). Essas fontes são referências de implementação, não substituem validação em hardware.
- **Limite:** a TMEM física completa, incluindo bancos e swizzle, continua pendente; a carga 8/16-bit ainda é um modelo funcional simplificado.


## 16.17 Correção da suíte LOAD_BLOCK após CI (2026-11-22)

- A primeira execução após a restrição dos formatos falhou na compilação porque a edição dos testes deixou uma chamada sem declaração correspondente.
- A execução seguinte compilou, mas falhou em um teste antigo de 4-bit que ainda esperava sucesso, comportamento agora explicitamente rejeitado.
- Reorganizada a suíte: teste de 8-bit e teste de 16-bit ficam em função LOAD_BLOCK própria; teste 4/32-bit verifica `Unsupported`; o teste LOAD_TILE não contém mais verificações de LOAD_BLOCK.
- A compilação Android havia passado; o bloqueio estava exclusivamente nos testes nativos. A correção precisa ser confirmada por nova execução de CI antes de marcar a etapa como aprovada.



## 16.18 Confirmação final do CI após reorganização dos testes (2026-11-22)

O workflow do commit `f9ea01dfbbf692af60d6ce5bd8ccf471f25686b1` concluiu com sucesso:
- `native-host-tests`: sucesso; configuração, compilação e execução dos testes nativos passaram;
- `assemble-debug`: sucesso; configuração do JDK/Gradle/Android SDK/NDK/CMake, compilação do APK e upload do artefato passaram;
- artefato publicado: `m64plus-debug-apk` (574.338 bytes, não expirado na verificação);
- execução: [GitHub Actions #45023007609](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45023007609).

A etapa de correção dos testes LOAD_TILE/LOAD_BLOCK está, portanto, validada pelo CI. Isso confirma compilação e testes automatizados do projeto; não comprova compatibilidade com jogos comerciais nem fidelidade em hardware real.

## 16.19 Revisão de DXT e limites da carga LOAD_BLOCK

Nova conferência das regras de LOAD_BLOCK:
- o manual confirma que DXT é acumulado uma vez por palavra de 64 bits; quando o acumulador avança para a próxima linha, o contador de linha aumenta e palavras de linhas ímpares têm as metades de 32 bits trocadas;
- a documentação também alerta que valores DXT não potência de dois podem acumular erro de linha; portanto, não assumir que o contador simplificado reproduz todos os casos sem testes de referência;
- o código atual incrementa DXT apenas para palavras completas de 8 bytes. Um fragmento final menor que 8 bytes não participa do acumulador nem da troca. Esse comportamento permanece explicitamente pendente de validação, não deve ser declarado fiel;
- a implementação usa buffer temporário e limita a escrita a 4 KiB, mas ainda não implementa o layout físico completo da TMEM, os bancos separados para texels de 32 bits, nem a semântica completa de formatos YUV/RGBA;
- LOAD_BLOCK de 4 bits permanece rejeitado no caminho comum; 32 bits permanece sem o caminho dedicado.

Referências consultadas nesta revisão: Nintendo 64 Programming Manual, seção 13.9.3 (LOAD_BLOCK); RDP Command Summary; documentação técnica RustyN64 e implementação Project64. Fontes de referência: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-09.html ; https://hcs64.com/files/RDP_COMMANDS.pdf ; https://github.com/doublegate/RustyN64/blob/main/docs/rdp.md ; https://github.com/project64/project64/blob/develop/Source/Project64-video/rdp.cpp .

**Próxima etapa recomendada:** construir testes independentes de DXT para múltiplos carries, linhas ímpares consecutivas e fragmentos finais; só depois alterar a rotina. Em seguida, implementar e testar o layout físico da TMEM (incluindo troca de halfwords/bancos), mantendo o sampler e rasterizador como etapas separadas.



## 16.20 Testes adicionais de DXT aprovados pelo CI (2026-11-22)

O commit `ce2a5d435a58db36c9fc808805bfbd1a4cca7d97` adicionou cenários de teste:
- DXT alto (`0xFFF`), que produz múltiplos carries por palavra e alternância de paridade das linhas;
- DXT fracionário (`0x600`), que acumula frações e inicia a troca de metades somente quando a linha passa a ser ímpar;
- preservação dos testes anteriores de DXT `0x400`, carga 8/16-bit e rejeição dos caminhos 4/32-bit não implementados.

**Resultado confirmado:** workflow [#45023292915](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45023292915) concluiu com `native-host-tests = success` e `assemble-debug = success`. O artefato `m64plus-debug-apk` foi publicado (574.338 bytes, não expirado na verificação).

**Interpretação:** os testes confirmam o comportamento da implementação atual para os casos cobertos, não uma comparação integral com hardware ou RDP de referência. O fragmento final menor que 64 bits, os casos de DXT fora dos vetores testados e a organização física de TMEM permanecem pendentes.



## 16.21 LOAD_TILE — troca das metades em linhas ímpares (2026-11-22)

A revisão da organização de TMEM identificou uma ausência no caminho LOAD_TILE: linhas ímpares de texels de 4/8/16 bits precisam trocar as duas metades de 32 bits dentro de cada palavra de 64 bits para permitir o acesso intercalado do filtro de textura.

Correção aplicada:
- o destino de cada byte de uma linha ímpar passa a usar o endereço com bit de meia-palavra trocado (`offset ^ 4`);
- o caminho de nibbles também aplica a troca ao byte que contém o texel;
- o teste de LOAD_TILE com duas linhas agora verifica que a segunda linha aparece no deslocamento swizzled (12/13 em vez de 8/9, para o caso de teste);
- o caminho de texel de 32 bits foi explicitamente bloqueado em LOAD_TILE: a cópia plana anterior não representava o layout dividido entre bancos/metades da TMEM e não deve ser tratada como suportada.

**Limitações que permanecem:**
- o caminho de 4 bits ainda é uma cópia lógica de nibbles e precisa de comparação específica com o comportamento documentado; o manual recomenda carregar tipos 4-bit usando transferência maior e configurar o tile de renderização para 4-bit;
- a distribuição física de texels 32-bit entre os bancos da TMEM não foi implementada;
- alinhamento de linhas, limites de TMEM, swizzle por formato e interação com sampler ainda precisam de testes dedicados;
- a nova correção aguarda confirmação do CI antes de ser considerada validada.

**Referências:** Nintendo 64 Programming Manual, seção 13.8.1 (organização de TMEM) e 13.9 (LOAD_TILE/LOAD_BLOCK); RustyN64 RDP notes (layout de TMEM comparado com ParaLLEl-RDP); Moria, “N64 Part 20: TMEM Format and Mip Maps”. Links: https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-09.html ; https://github.com/doublegate/RustyN64/blob/main/docs/rdp.md ; https://www.moria.us/blog/2020/11/n64-part20-tmem-format-and-mip-maps .


## 16.22 Testes de regressão adicionais do LOAD_TILE (2026-11-22)

- Ampliada a suíte para cobrir a troca das metades de 32 bits em linhas ímpares com texels de 16 bits, incluindo verificação da ordem big-endian e do stride de origem da imagem.
- Adicionado teste que confirma que LOAD_TILE de 32 bits retorna `Unsupported` e não altera a TMEM, evitando aceitar silenciosamente uma cópia linear incorreta.
- A configuração do teste 16-bit foi revisada em duas partes: a segunda linha da origem começa em `base + image.width * 2`, e as coordenadas TL/TH do comando foram ajustadas para selecionar duas linhas (TL=0, TH=4), não apenas a segunda linha.
- **Status:** testes adicionados ao branch `main`; aguardando CI do commit de correção. Não considerar esta etapa validada até `native-host-tests` e `assemble-debug` concluírem.
- **Próxima investigação:** implementar o caminho especial de 32 bits somente após definir e testar o mapeamento de bancos/metades TMEM e o comportamento de LOAD_TILE versus LOAD_BLOCK. A rejeição explícita permanece intencional.


## 16.23 Primeiro caminho RGBA32 de LOAD_TILE (2026-11-22)

Com base na descrição técnica do layout de 32 bits no RDP, foi implementado um primeiro caminho específico para `LOAD_TILE`:
- aceita somente combinação RGBA + 32 bits tanto na imagem-fonte quanto no tile;
- separa R/G em pares de bytes na metade inferior de 2 KiB da TMEM e B/A na metade superior (offset `0x800`);
- aplica a troca de meia-palavra de 32 bits em linhas ímpares em cada metade;
- valida todos os bytes da região-fonte antes de alterar a TMEM;
- mantém `LOAD_BLOCK` de 32 bits explicitamente não suportado; não reaproveita a cópia plana de 8/16 bits.

Foi adicionada uma regressão de duas linhas e dois texels por linha, que verifica os pares RG/BA nas duas metades da TMEM e o swizzle da segunda linha.

**Estado:** implementação e teste enviados à `main`; aguardando CI. A distribuição RG/BA é baseada na documentação técnica RustyN64 e no caminho especial `LoadTile32b` identificado no Project64. Ainda é uma primeira implementação lógica: precisa de validação independente contra um RDP de referência/hardware, casos de TMEM com base não zero, larguras/alinhamentos variados e formatos não RGBA. O sampler, rasterizador e caminho LOAD_BLOCK32 continuam pendentes.

Referências: https://github.com/doublegate/RustyN64/blob/main/docs/rdp.md ; https://github.com/project64/project64/blob/develop/Source/Project64-video/rdp.cpp .



## 16.24 LOAD_TILE RGBA32 — base TMEM diferente de zero e CI (2026-11-22)

- O workflow [#45024077569](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45024077569), no commit de código `8a8196b0de2b5c84d75fff9325be32b904699415`, concluiu com sucesso: `native-host-tests` = success e `assemble-debug` = success.
- O artefato `m64plus-debug-apk` foi publicado, com 576.559 bytes, não expirado na verificação.
- O teste RGBA32 foi ampliado para usar `tile.tmem = 2` (base de 16 bytes), verificando que os pares RG são escritos a partir do deslocamento 16, os pares BA a partir de 0x810 e que o swizzle da segunda linha é preservado nas duas metades.
- Essa regressão cobre agora uma base TMEM não zero, mas somente um valor pequeno e alinhado. Não valida limites superiores, wrap, sobreposição entre bancos ou todos os valores possíveis de TMEM.
- **Status:** o novo teste foi enviado à `main` no commit `f91238b47cc1ab54022e51eec43b20eb6a1162f2`. O resultado do CI desse commit ainda precisa ser consultado antes de declarar o teste aprovado.
- **Limite:** a separação RG/BA e o swizzle continuam sendo uma implementação inicial baseada em documentação/código de referência; falta comparação independente com hardware ou RDP de referência. `LOAD_BLOCK` 32-bit, sampler e rasterizador continuam pendentes.


## 16.25 LOAD_TILE RGBA32 — rejeição de formatos incompatíveis (2026-11-22)

- Adicionado teste para duas combinações inválidas: imagem-fonte com formato diferente de RGBA e tile com formato diferente de RGBA, mantendo o tamanho de 32 bits.
- Em ambos os cenários, o fluxo deve retornar `Unsupported` no LOAD_TILE e manter inalterados os bancos inferior e superior da TMEM.
- O teste foi enviado à `main` no commit `02f1472a64ab7745e70b2b195e4616c4ab6e5172`; aguarda CI.
- Referência de formato: a tabela de atributos de tile do manual define RGBA como formato 0 e 32 bits como tamanho 3. A implementação de 32 bits aqui permanece restrita a RGBA.
- **Limite:** este teste não cobre os limites de endereço TMEM, wrap, stride grande, nem valida o resultado contra hardware. Não ampliar suporte de formatos sem vetores de referência independentes.


## 16.26 LOAD_TILE RGBA32 — CI do teste de formatos incompatíveis (2026-11-22)

- O workflow [#45024695074](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45024695074), para o commit de código `02f1472a64ab7745e70b2b195e4616c4ab6e5172`, concluiu com sucesso.
- `native-host-tests`: success. `assemble-debug`: success. A etapa de upload do APK também concluiu com sucesso.
- Artefato `m64plus-debug-apk`: 576.561 bytes; não expirado na consulta de 2026-11-22. Download disponível na seção Artifacts do workflow.
- O resultado confirma que os testes automatizados e a compilação Android passam para esse commit. **Não** comprova compatibilidade com jogos, precisão completa do RDP, nem funcionamento de renderização de ROMs.
- Próxima etapa: fortalecer o teste de rejeição com valores sentinela previamente gravados na TMEM e investigar limites/endereço de destino do caminho RGBA32 sem alterar a lógica até existir referência técnica independente.


## 16.27 Teste de não alteração da TMEM com sentinelas (2026-11-22)

- O teste de rejeição de formatos incompatíveis no LOAD_TILE RGBA32 foi fortalecido: antes do comando inválido, grava quatro bytes sentinela nas duas metades da TMEM (endereços 0/1 e 0x800/0x801).
- Após o resultado `Unsupported`, o teste exige que os quatro valores originais permaneçam intactos. Isso verifica ausência de escrita parcial/sobrescrita nos pontos amostrados, em vez de apenas confirmar que a memória inicialmente zerada continuou zerada.
- Alteração enviada à `main` no commit `9029eaabb85a5eda2136212d0cb7610b716883a3`; CI ainda precisa ser consultado.
- **Escopo:** o teste cobre os dois bancos em quatro endereços específicos. Não prova atomicidade para toda a TMEM nem substitui testes de limite, wrap e validação externa.


## 16.28 Correção de wrap independente dos bancos TMEM em LOAD_TILE RGBA32 (2026-11-22)

- Revisão do cálculo de endereço encontrou um caso-limite: o endereço do banco superior era derivado do endereço lógico ainda não mascarado. Quando a linha ultrapassava 0x7FF, o cálculo podia fazer os bytes B/A voltarem para a metade inferior da TMEM e sobrescrever R/G.
- Correção: primeiro normalizar o endereço na metade inferior (`logical/swizzle & 0x07FF`); depois derivar o endereço correspondente da metade superior somando `0x0800`. O wrap de cada metade passa a ser independente.
- Novo teste com `tile.tmem = 255`, `line = 1` e duas linhas verifica a última posição da metade inferior/superior e a linha ímpar que cruza o limite. Confere que RG e BA permanecem separados.
- Código enviado à `main`: `e693b2cf918a0f669afc0aa569c686442c8f5c36`; teste enviado à `main`: `48395912d256e9922bb2884794594b9be6ec1287`.
- **Status:** correção ainda não considerada validada até o CI concluir. A prova atual é um teste unitário determinístico; ainda falta comparação contra ParaLLEl-RDP/Angrylion ou hardware para todos os alinhamentos e limites.
- Referência usada para a regra de endereço: RustyN64 RDP notes, seção “The TMEM loads”, que descreve a separação RG/BA e o mascaramento em 0x7FF: https://github.com/doublegate/RustyN64/blob/main/docs/rdp.md .


### Ajuste do vetor de limite RGBA32

Ao revisar o novo vetor antes do CI, foi corrigido o campo WIDTH de SET_TEXTURE_IMAGE: o campo codifica largura menos um, portanto o teste de uma coluna deve gravar zero nesse campo. Sem esse ajuste, a segunda linha apontaria para além dos oito bytes de origem preparados. Correção enviada no commit `709b23f47fd7e22e7a2600e38e17c89584ee3c72`; o CI desse commit é o que deve ser usado para validar o teste final.


### Segundo ajuste do vetor de limite RGBA32

A execução do vetor corrigido ainda encontrou um campo de coordenada incorreto: `SH` estava em 4 (duas colunas), quando o caso de uma coluna exige `SH=0`; `TH=4` seleciona a segunda linha. O comando foi ajustado para codificar somente `TH=4`. Commit: `289952bf153606a09eecd1b06d644d7bb669bad8`. A próxima execução deste commit deve determinar se o vetor passa; não tratar execuções anteriores como validação da correção.


### Isolamento das coordenadas nos testes de LOAD_TILE

Na revisão do vetor de limite, o CI revelou que a substituição anterior também havia alterado por engano o comando do teste de regressão 16-bit (que precisa de duas colunas). O teste 16-bit foi restaurado para `SH=4, TH=4`; somente o vetor RGBA32 de uma coluna usa `SH=0, TH=4`. Correção enviada no commit `86ec0bb1e0405b471c6d9a8e1d632e05cf219f6f`. Os resultados dos commits anteriores permanecem falhos e não contam como validação.


## 16.29 CI aprovado após correções do vetor RGBA32 (2026-11-22)

- Workflow [#45024990879](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45024990879), commit de teste `86ec0bb1e0405b471c6d9a8e1d632e05cf219f6f`: `native-host-tests` success e `assemble-debug` success; upload do APK success.
- Artefato `m64plus-debug-apk`, ID `14755820040`, tamanho 576.614 bytes, não expirado na consulta de 2026-11-22.
- Esse CI valida o teste de regressão 16-bit restaurado e o teste RGBA32 de wrap, além dos demais testes automatizados e compilação Android. Não valida precisão de hardware nem compatibilidade com ROMs comerciais.


## 16.30 Teste dedicado de LOAD_TILE 4-bit (2026-11-22)

- Adicionado teste com imagem CI/4-bit de quatro texels por linha e duas linhas, verificando empacotamento de nibble alto/baixo e troca dos dois blocos de 32 bits na linha ímpar.
- Vetor esperado: primeira linha em TMEM 0x00/0x01 (`0x12, 0x34`); segunda linha em 0x0C/0x0D (`0x56, 0x78`) com `line=1`.
- Teste enviado à `main` no commit `41aa27c14ad76e9950d4a2bd187ea85d7a7047f2`. CI pendente.
- Limitação: esse teste confirma o modelo implementado de nibble packing e odd-row swizzle, mas não estabelece sozinho que a transferência direta 4-bit reproduz todo comportamento de hardware; comparar com implementação de referência continua necessário.


### Ajuste do vetor de LOAD_TILE 4-bit após CI

- O primeiro CI do teste 4-bit falhou porque o comando de teste colocou `4` no campo TL (coordenada inicial T), em vez de TH (coordenada final T). Assim, o teste lia a linha de origem errada; não era evidência de falha no empacotamento de nibble da implementação.
- Corrigido o comando para TL=0, TH=4, mantendo SL=0 e SH=12. Commit: `050febdbb3028a29a881f4e0f9d9cfa201c74045`.
- O teste ainda aguarda novo CI; não declarar LOAD_TILE 4-bit validado até o resultado.


## 16.31 CI aprovado para LOAD_TILE 4-bit (2026-11-22)

- Workflow [#45025229147](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45025229147), commit de teste `050febdbb3028a29a881f4e0f9d9cfa201c74045`: `native-host-tests` success e `assemble-debug` success; etapa de upload do APK success.
- O teste 4-bit corrigido (TL=0, TH=4) passou junto com toda a suíte nativa. A compilação Android também passou.
- Artefato `m64plus-debug-apk`, ID `14754758311`, tamanho 576.611 bytes, SHA-256 `f9c79c03dd592e89a8f8e02ba5e81587f0504d9a9f5eec6da2880dc89bd43592`, expiração prevista em 2027-02-20. Download na seção Artifacts do workflow.
- O workflow seguinte, disparado pelo commit de auditoria, também concluiu com sucesso: [#45025241020](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45025241020), com `native-host-tests` e `assemble-debug` success.
- **Escopo da validação:** confirma testes automatizados e compilação. Não demonstra que LOAD_TILE 4-bit tenha fidelidade completa em hardware, nem que o emulador renderize jogos.
- Próxima frente proposta: desenhar uma interface explícita de framebuffer entre a memória de imagem do RDP e o renderer Android. Antes de integrar, especificar formato de pixel, stride/largura, altura, sincronização de thread e propriedade do buffer; manter a tela de diagnóstico como fallback até existir framebuffer válido.


## 17.1 Primeira estrutura de framebuffer RDP (2026-11-22)

- Criada a classe `RdpFramebuffer` em `app/src/main/cpp/n64/framebuffer.h/.cpp` para converter uma região de color image da RDRAM em pixels `0xAARRGGBB`, formato conveniente para a futura ponte JNI/Android.
- Formatos inicialmente aceitos: RGBA16 (5551) e RGBA32 (8888), ambos com leitura big-endian por meio da classe RDRAM existente. RGBA16 expande os canais de 5 bits para 8 bits e converte o bit alpha; RGBA32 reorganiza RGBA para ARGB.
- A função recebe explicitamente a altura porque o descritor SET_COLOR_IMAGE fornece a largura, mas não a altura. A área é tratada como linhas contíguas com stride igual à largura; ainda não há suporte a stride separado, formatos CI/IA/I/YUV, resolução dinâmica automática ou sincronização com a GPU.
- Validação defensiva: formato, tamanho, dimensões máximas (1024x1024), quantidade de pixels e faixa completa na RDRAM. O buffer de saída só é substituído depois que toda a conversão termina com sucesso.
- Criado `tests/framebuffer_tests.cpp` cobrindo cores RGBA16, conversão RGBA32, ordem de bytes, alpha, formato inválido, dimensões excessivas e endereço fora da RDRAM. Registrado novo alvo CTest `m64plus_framebuffer_tests` e inclusão do novo arquivo na biblioteca Android.
- **Status:** implementação enviada à `main`; aguarda CI. Mesmo após CI verde, isso será somente o conversor de memória para pixels. Ainda falta conectar a instância real de RDRAM/RDP da máquina ao JNI, enviar o buffer para a thread GL e substituir o triângulo por textura. Não há rasterizador RDP nem renderização de jogos nesta etapa.


## 17.2 CI aprovado e auditoria da integração Android do framebuffer (2026-11-22)

- Workflow [#45026072223](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45026072223), commit de auditoria `be39c8c501cbb0fab9515a7dcb572759a8e3b3cf`: `native-host-tests` e `assemble-debug` concluíram com sucesso; a etapa de upload do APK também passou.
- Artefato `m64plus-debug-apk`, ID `14755846775`, tamanho 597.806 bytes, SHA-256 `f0b20e1f0c1f3f198dcc339a477abfe78899dd7e965557c5e8a0a2bd87df8b30`, expiração prevista em 2027-02-20.
- Inspeção de `MainActivity.java` e `BootRenderer.java` confirma que a tela ainda é o triângulo de diagnóstico GLES 2.0. Não há método JNI de obtenção do framebuffer nem upload de textura; o renderer não consome pixels do núcleo N64.
- `native-lib.cpp` contém atualmente vários self-tests JNI, não uma instância de máquina persistente para execução de ROM. `N64Machine` agrega Bus/CPU/RSP, mas ainda não possui `RdpSyncState` nem ciclo de processamento RDP. O `Bus` já expõe `rdram()`, permitindo futuramente ler a imagem da RDRAM sem duplicar memória.
- Próxima etapa técnica: definir a propriedade e o ciclo de vida de uma máquina nativa persistente, agregar estado RDP à máquina e fornecer uma API JNI segura para obter um snapshot de pixels. Depois, o renderer poderá fazer upload para textura GLES na própria thread GL. Não remover o triângulo de diagnóstico até existir imagem válida.
- **Limitação funcional atual:** o conversor de framebuffer e seus testes estão integrados ao build, mas ainda não há uma fonte real de framebuffer produzida por comandos de desenho RDP; portanto, esta versão não renderiza jogos nem substitui o triângulo por uma imagem de ROM.


## 17.3 Estado RDP integrado à máquina compartilhada (2026-11-22)

- `N64Machine` agora possui `RdpSyncState` como parte do estado persistente da máquina, além de Bus, CPU principal e RSP. `reset()` limpa também o estado RDP.
- Adicionada `runRdp(commandBudget)`: consome no máximo o orçamento informado, usando `RdpCommandProcessor::processNext`. Comandos não suportados continuam sendo reportados pelo processador e são consumidos, mas não são tratados como renderizados.
- Adicionados testes de integração que escrevem `SET_COLOR_IMAGE` no fluxo DPC, executam a máquina e verificam formato RGBA, tamanho 16-bit, largura 320, endereço 0x2000 e limpeza após reset.
- `rdp.cpp` foi incluído no alvo de testes da máquina no CMake raiz.
- Commits na `main`: `8c361f836f105d1a0183f4d01b79ea4edc07b52b` (interface), `708b165138c05280a88eb07f1541c4f4d8b4a74b` (implementação), `26b478360c6d74c118aea3e5156c93ebffc427a4` (teste) e `08d00b33ba8f3a5b830a678afb91a43b0ee35ac9` (CMake).
- **Escopo:** a máquina agora conserva e processa comandos RDP que já têm implementação de estado/carga. Triângulos, retângulos de textura e demais comandos de desenho ainda não rasterizam pixels; este passo não cria framebuffer renderizado nem altera a tela Android.
- **Validação:** aguarda o CI do commit mais recente; não declarar os novos testes aprovados até o workflow terminar.


## 17.4 Ciclo de vida JNI da máquina nativa persistente (2026-11-22)

- `native-lib.cpp` agora expõe operações JNI para criar, destruir, reiniciar e consultar o PC da CPU de uma instância `N64Machine` persistente. A alocação usa `std::nothrow` ; chamadas de reset/consulta verificam handle nulo.
- `MainActivity` cria a máquina durante `onCreate`, registra no Logcat o estado de criação e o PC inicial, e libera a instância em `onDestroy`.
- Commits na `main`: JNI `a7044f99aa3a859750a55fe4257d76dceea80f18`; integração Android `ef3628067c12e0e113b296464eb8fe99e4398805`.
- Workflows acionados: [#45048490439](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45048490439) e [#45048493316](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45048493316). Nos dois, `native-host-tests` concluiu com sucesso; `assemble-debug` ainda estava em execução no momento desta atualização. O resultado Android deve ser confirmado antes de declarar a ponte validada.
- **Escopo:** a atividade agora mantém a instância nativa durante sua vida útil, mas ainda não executa CPU/RSP em loop, não carrega ROM, não conecta framebuffer ao renderer e não rasteriza comandos RDP. O triângulo GLES continua sendo apenas diagnóstico.
- **Próximas etapas:** validar o build Android; definir sincronização e orçamento de execução entre thread de emulação e thread GL; depois implementar carregamento de ROM/boot e produzir pixels reais antes de substituir o triângulo.

## 17.5 Correção de sintaxe Java no ciclo de vida JNI (2026-11-23)

- A inspeção dos logs dos workflows revelou a causa das falhas Android: `MainActivity.java` continha sequências literais `\\n` inseridas no código-fonte em vez de quebras de linha reais. O compilador Java reportou `illegal character: '\\'` e erros de sintaxe nas linhas 54 e 67.
- Corrigido o arquivo substituindo as 18 sequências literais por quebras de linha reais, sem alterar as chamadas JNI nem a lógica do ciclo de vida.
- Commit de correção na `main`: `d22c49858035c17c929e80201b6c55556cb890ba`.
- Workflow [#45113502543](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45113502543), commit `d22c49858035c17c929e80201b6c55556cb890ba`: `native-host-tests` e `assemble-debug` concluíram com sucesso; o upload do APK também passou. Artefato `m64plus-debug-apk`, ID `14790960275`, tamanho 605.696 bytes, expira em 2027-02-21.
- Os workflows anteriores #45048493316 e #45048552767 falharam na compilação Android por esse erro; seus resultados não validam a integração Java. O workflow #45048490439 passou antes da integração Java no `MainActivity`, portanto não valida essa etapa.
- **Limitação funcional permanece:** a máquina persistente é criada e destruída com a atividade, mas ainda não há loop de emulação, carregamento de ROM, execução coordenada CPU/RSP, framebuffer produzido por rasterização RDP ou upload de textura no renderer. O triângulo continua sendo apenas diagnóstico.


## 17.6 Primeiro desenho de pixels RDP: FILL_RECTANGLE RGBA32 (2026-11-23)

- Implementado o primeiro caminho de escrita de pixels pelo RDP: comando `FILL_RECTANGLE` (0x36), limitado deliberadamente ao modo FILL (cycle type 3), color image RGBA32 e SET_FILL_COLOR previamente configurados.
- As coordenadas 10.2 são convertidas para pixels; em modo FILL, os limites do retângulo são inclusivos. Quando SET_SCISSOR está presente, o retângulo é recortado antes da escrita. O caminho grava o valor de 32 bits em ordem big-endian na RDRAM, usando a largura de SET_COLOR_IMAGE como stride.
- A implementação valida toda a região de destino antes da primeira escrita para evitar framebuffer parcialmente alterado por endereço fora da RDRAM.
- Adicionado teste de integração que configura color image 4x4, fill color, modo FILL e retângulo 1..2 por 1..2; verifica os quatro pixels e os sentinelas externos. Um segundo caso verifica recorte por scissor.
- **Limitações:** somente RGBA32; sem RGBA16/8/4; sem ciclos 1/2, blender, antialiasing, coverage, Z, dithering ou rasterização de triângulos; altura continua implícita e limitada pela RDRAM; a interpretação de scissor é uma primeira implementação e precisa de vetores independentes de referência. O framebuffer ainda não foi ligado ao JNI/renderer GLES, portanto o triângulo de diagnóstico Android permanece.
- Referências técnicas: manual N64 para coordenadas e semântica inclusiva em FILL mode (<https://ultra64.ca/files/documentation/online-manuals/man/n64man/gdp/gDPFillRectangle.html>); descrição de FILL pipeline, repetição do registrador de 32 bits e scissor (<https://github.com/doublegate/RustyN64/blob/main/docs/rdp.md>); layout do comando (<https://hcs64.com/files/RDP_COMMANDS.pdf>).
- **Status:** código e testes enviados à `main`; aguardar CI antes de considerar esta etapa validada.


### Correção do vetor 10.2 do FILL_RECTANGLE

- O primeiro CI do novo teste RDP detectou falha em `m64plus_rdp_tests`: o retângulo de teste usava valores de coordenadas 1 e 2 diretamente, mas o comando codifica coordenadas em formato 10.2 (quatro unidades por pixel).
- Corrigido o vetor para XH/YH = 4 e XL/YL = 8, que representam os pixels 1 e 2. A falha era do vetor de teste; a execução não foi considerada validação da implementação.
- Também foi reforçada a escrita RGBA32 para emitir bytes big-endian por `write8`, sem depender do alinhamento de 32 bits do endereço-base da color image.
- Commit de correção do vetor e resultado do CI correspondente ainda pendentes.


## 17.7 Integração de teste RDP → framebuffer ARGB8888 (2026-11-23)

- O teste de `FILL_RECTANGLE` agora passa o mesmo color image RGBA32, já escrito pelo RDP na RDRAM, diretamente para `RdpFramebuffer::copyToArgb8888`.
- Verifica o número de pixels e a conversão dos canais/alpha do pixel desenhado e do sentinela de fundo.
- Isso valida a ligação em memória entre escrita do RDP e conversor de framebuffer em teste host. **Ainda não é a ligação JNI/OpenGL da tela Android**: não foi adicionado loop de emulação, chamada JNI para obter pixels nem upload de textura GLES; o triângulo de diagnóstico continua na tela.
- CI a executar para esta integração.


### Correção de linkage do teste RDP/framebuffer

- O primeiro CI da integração RDP → conversor falhou no link de `m64plus_rdp_tests`: o alvo não compilava `framebuffer.cpp`, embora o teste passasse a chamar `RdpFramebuffer::copyToArgb8888`.
- Adicionado `app/src/main/cpp/n64/framebuffer.cpp` às fontes do alvo `m64plus_rdp_tests` no CMake raiz.
- A falha era de configuração do alvo de teste, não do código do conversor. A nova execução CI deve validar build, teste e APK.


### Resultado CI: integração RDP → framebuffer (2026-11-23)

- Workflow [#45114354917](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45114354917), commit `af6d046fc5c27c7b06af1022a0122aebc79b620f`: ambos os jobs concluíram com sucesso — `native-host-tests` (build e execução dos testes nativos) e `assemble-debug` (compilação Android).
- O sucesso valida o teste host que exercita a escrita de pixels RGBA32 por `FILL_RECTANGLE` e a conversão subsequente para ARGB8888, além da compilação do APK de depuração. Não valida exibição na tela, carregamento/execução de ROMs ou compatibilidade com jogos.
- Artefato gerado pelo workflow: `m64plus-debug-apk`, ID `14791530537`, tamanho 607.872 bytes, ainda não expirado na data desta auditoria. Download oficial: <https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45114354917> (seção Artifacts).
- **Próxima etapa técnica:** inspecionar JNI, ciclo de vida da máquina e renderer antes de projetar a transferência do framebuffer para textura GLES. Preservar o triângulo de diagnóstico como fallback até existir um frame real válido.


## 17.8 Ponte JNI e apresentação GLES do framebuffer (2026-11-23)

- Adicionada a JNI `nativeGetFramebufferSnapshot(handle)`: valida o handle, exige SET_COLOR_IMAGE válido, limita a largura a 1024 e solicita ao conversor CPU um snapshot RGBA16/32 para uma altura provisória fixa de 240 linhas. Retorna um array Java com largura, altura e pixels ARGB8888; retorna null quando ainda não há imagem válida.
- O renderer GLES 2.0 agora mantém o triângulo colorido como fallback e cria um segundo programa de shaders para exibir uma textura RGBA em tela cheia quando a JNI entregar um snapshot válido. Os canais ARGB são explicitamente reorganizados em RGBA antes do upload; a primeira apresentação é registrada no Logcat como `NATIVE_FRAMEBUFFER_DISPLAYED`.
- `MainActivity` passa o handle persistente e um lock ao renderer. O acesso ao snapshot e a destruição da máquina compartilham esse lock; a superfície GL é pausada antes da liberação no ciclo de destruição para evitar uso do ponteiro após delete.
- **Ainda não é uma imagem de jogo:** não há loop CPU/RSP, carregamento de ROM, boot de cartucho nem comandos RDP gerados por execução de jogo. Sem SET_COLOR_IMAGE e bytes de framebuffer válidos, a aplicação continua mostrando o triângulo. A altura de 240 é provisória e precisa ser derivada do modo VI/estado real; a cópia de um array e o buffer de upload a cada frame também são temporários e precisam de otimização.
- **Validação pendente:** os três arquivos foram enviados à `main`; aguardar os jobs Android e host antes de considerar a ponte validada.


### Resultado CI: ponte JNI e renderer GLES (2026-11-23)

- Workflow [#45114873619](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45114873619), commit `88d93f9b12298d152776bf036f3e861ba77b518d`: `native-host-tests` e `assemble-debug` concluíram com sucesso. A etapa `Upload debug APK` também passou.
- Artefato deste commit: `m64plus-debug-apk`, ID `14791131799`, tamanho 610.908 bytes, SHA-256 `3929f98c4cc0c160d5880f8691ba8d63b9a82ca4539909842f60bcf36d620012`, expira em 2027-02-21. Disponível na seção Artifacts do workflow.
- **Conclusão de validação:** o código C++/JNI e o renderer Android GLES compilam; a suíte nativa configurada passou. Isso não comprova que um frame de jogo seja produzido ou mostrado em dispositivo.
- **Limitação funcional permanece:** ainda não há carregador de ROM/cartucho, boot funcional ou loop que execute CPU/RSP e alimente o RDP. Portanto, a chamada de snapshot pode continuar retornando null e o renderer deve manter o triângulo de diagnóstico.
- Próxima etapa: revisar o caminho de execução e inicialização da máquina e implementar, em etapas testáveis, carregamento de ROM e avanço controlado da CPU/RSP antes de esperar uma imagem de jogo.


## 17.9 Reutilização do buffer de upload GLES (2026-11-23)

- Revisado o caminho de upload de snapshots em `BootRenderer`. Antes, `uploadSnapshot` alocava um novo `ByteBuffer.allocateDirect` a cada frame, mesmo quando a resolução não mudava.
- O renderer agora mantém um `ByteBuffer` direto como campo e reutiliza sua capacidade quando suficiente; só realoca se uma resolução maior exigir mais bytes. O buffer é limpo, preenchido com os canais RGBA e reposicionado antes de `glTexSubImage2D`.
- Commit na `main`: `56d0363402372dd22653a505a2f0f78d7cee4823`.
- **Escopo:** reduz alocações diretas repetidas no lado Java quando houver framebuffer válido. Não remove a alocação do snapshot JNI nem a conversão ARGB→RGBA, e não muda a ausência de ROM/boot/loop de emulação. Ainda precisa ser validado por CI e medido em dispositivo real.


## 17.10 Base de mapeamento de cartucho N64 (2026-11-23)

- `Bus` agora aceita carregar uma imagem ROM big-endian no formato `.z64` por `loadCartridge(data, size)`.
- Validação: ponteiro não nulo, tamanho mínimo de cabeçalho (0x40), máximo de 64 MiB, múltiplo de quatro bytes e magic `0x80371240`. Falha não substitui cartucho anteriormente carregado.
- ROM é mapeada como região somente leitura no início da janela física PI/cartucho `0x10000000`; acessos fora do tamanho real carregado são rejeitados. `reset()` preserva o cartucho inserido; `unloadCartridge()` remove o mapeamento.
- Teste host cobre cabeçalho e palavras big-endian, escrita rejeitada, leitura além do tamanho, rejeição de magic inválido, preservação após falha, reset e unload.
- Commits na `main`: header `a6d0faa`, implementação `ed8b45d`, testes `47d2716`.
- **Limitação importante:** isso ainda não é um seletor de ROM Android nem boot de cartucho. Formatos `.v64` e `.n64` não são convertidos; PI DMA não está implementado; não há configuração de CIC/boot code/seed, checksum ou execução do IPL3. A API recebe bytes já carregados e somente mapeia a ROM.
- CI para os commits e a auditoria ainda pendente.


### Correção de CI da região Cartridge

- O primeiro CI do commit do cabeçalho detectou que os novos valores `BusRegion::Cartridge` não estavam tratados nos dois switches de acesso por byte; `-Werror=switch` corretamente interrompeu a compilação.
- Correção aplicada em `bus.cpp` no commit `96a13c4`: leitura de bytes consulta a ROM carregada e escrita retorna `false` (ROM somente leitura).
- A execução que revelou o erro: [#45115475958](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45115475958). O CI dos commits posteriores deve confirmar a correção.


- **Segunda correção do CI:** a primeira aplicação da correção duplicou os dois `case BusRegion::Cartridge`; o compilador apontou `duplicate case value` no workflow [#45115527953](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45115527953). Duplicatas removidas no commit `ea7de8a`. A validação final segue pendente.


### Resultado CI: suporte inicial a cartucho `.z64` (2026-11-23)

- Workflow [#45115577480](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45115577480), commit `306ccc48f313f3f05a9058d263dc0bdf45bfe775`: os dois jobs passaram — `native-host-tests` e `assemble-debug`. O primeiro executou a suíte nativa; o segundo compilou o APK Android.
- Artefato `m64plus-debug-apk`, ID `14790929861`, tamanho 619.776 bytes, SHA-256 `b0f0c7f872099d01c8a9e2d2ce5f0642ac97958db78b272f8239135fa13782f3`, expira em 2027-02-21. Disponível na seção Artifacts do workflow.
- Revisão do caminho de endereço confirmou que KSEG1 virtual `0xB0000000` é convertido para endereço físico `0x10000000`, a janela inicial do cartucho. A CPU pode, portanto, buscar instruções da ROM por esse mapeamento, mas isso ainda não constitui boot completo.
- Adicionado teste de integração em `tests/machine_tests.cpp`: carrega uma ROM de teste `.z64`, posiciona duas instruções MIPS no offset `0x40` e verifica que a CPU as busca/executa a partir de `0xFFFFFFFFB0000040` (KSEG1), atualizando os registradores e o PC esperados.
- Commit do novo teste: `7f794b635f2047937af8250126ceb93e305426cc`. O workflow [#45115918716](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45115918716), no commit de auditoria `520e1bdb6e0e09c227a9abe91d1aa9739b74f30a` que já contém o teste, passou em `native-host-tests` e `assemble-debug`; assim, o teste de fetch/execução KSEG1 foi validado pela suíte nativa e o APK também compilou.
- Artefato desse workflow: `m64plus-debug-apk`, ID `14791762179`, 619.781 bytes, SHA-256 `e2c3a8e137bb776a783cd45ac2a3c37b46ed5c51f172ac79a98d92fd18a57042`, expira em 2027-02-21.
- **Limitações funcionais:** ainda não existe seletor Android/Storage Access Framework, ponte JNI para enviar bytes de ROM ao `Bus::loadCartridge`, PI DMA, sequência CIC/boot IPL3 real, checksum ou execução de jogos. O teste usa instruções artificiais no offset IPL3 apenas para validar o caminho de fetch da CPU.


## 17.11 Primeiro carregador de ROM no Android (2026-11-23)

- Adicionada JNI `nativeLoadCartridge(handle, byte[])`: valida handle/array/tamanho máximo e encaminha os bytes para `Bus::loadCartridge`. O buffer JNI é liberado com `JNI_ABORT`, pois o Bus mantém sua própria cópia.
- `MainActivity` agora apresenta o botão `ABRIR ROM (.z64)` sobre a tela OpenGL e abre o seletor de documentos Android (`ACTION_OPEN_DOCUMENT`). O arquivo é lido em thread de trabalho, limitado a 64 MiB, e carregado sob o mesmo lock usado pelo snapshot/destruição da máquina.
- O usuário recebe mensagem de sucesso/rejeição. A mensagem deixa explícito que carregar a ROM não inicia o jogo: ainda não há boot completo, PI DMA nem loop de CPU/RSP.
- A seleção usa o Storage Access Framework, sem pedir permissão ampla de armazenamento. A validação do núcleo continua aceitando apenas `.z64` big-endian com magic correto; a extensão do nome não substitui a validação binária.
- Commits na `main`: JNI `8b73e5965ea2c65080b863ec7481eb2a7f8c969e`; interface/seletor `bc512f2751d785189004b6409ebd7f19f5bfd6aa`.
- **Validação pendente:** aguardar CI nativo e Android para confirmar a compilação da JNI e da interface Java. Também falta teste manual no dispositivo para verificar o seletor e a leitura real de um arquivo `.z64`.
- **Limitações:** nenhum botão de iniciar/pausar jogo; nenhuma conversão `.v64`/`.n64`; nenhuma persistência da ROM após fechar o app; não iniciar CPU automaticamente para evitar saltar para um estado de boot ainda incompleto.


### Validação CI da integração Android de ROM (2026-11-23)

- JNI `nativeLoadCartridge` validada no workflow [#45115990178](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45115990178): `native-host-tests` e `assemble-debug` concluíram com sucesso.
- Seletor Android/SAF validado no workflow [#45116024737](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45116024737): ambos os jobs concluíram com sucesso.
- Auditoria final e estado combinado validados no workflow [#45116103405](https://github.com/jovemaprendz55-byte/M64plus/actions/runs/45116103405): ambos os jobs concluíram com sucesso.
- APK `m64plus-debug-apk`, artefato ID `14791966190`, tamanho 624.263 bytes, SHA-256 `e632dc8b1c4307874f3f0338aeba1878e80c85559ddfd84e62332de9f7bb0c82`, expira em 2027-02-21.
- A validação CI comprova compilação e testes nativos, não execução manual no Android. Falta selecionar ROM real no dispositivo e verificar mensagem de sucesso; nenhum jogo inicia ainda.
