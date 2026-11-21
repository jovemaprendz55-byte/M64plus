# M64plus

Emulador Nintendo 64 para Android. Plataforma inicial de desenvolvimento: Android 16, ARM64, com testes previstos no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

**Marco 0 — inicialização gráfica Android/OpenGL ES 2.0**

O aplicativo agora contém a estrutura Android mínima e um renderer de diagnóstico que desenha um triângulo colorido via `GLSurfaceView`. Esse marco serve para validar a criação do contexto OpenGL ES e a apresentação contínua de frames na tela.

> O triângulo é apenas um teste do pipeline gráfico. Ainda não existe CPU MIPS, RSP, RDP, áudio, carregador de ROM ou emulação de jogos.

## Abrir o projeto

Abra a pasta raiz no Android Studio com suporte ao Android Gradle Plugin 8.13.2, JDK 17 e Android SDK 36. O Gradle poderá solicitar a instalação do SDK correspondente.

## Próximas etapas

1. Compilar e executar o teste gráfico em um dispositivo Android.
2. Registrar GL_VENDOR, GL_RENDERER, GL_VERSION e extensões EGL/OpenGL ES.
3. Implementar o esqueleto nativo JNI e os testes de memória/CPU.
4. Desenvolver a CPU MIPS/R4300 e o sistema de memória.
5. Implementar RSP e RDP com sincronização correta.
6. Integrar áudio, controles e carregamento de ROMs.

O repositório anterior `N64-Android` é independente e não é alterado por este projeto.
