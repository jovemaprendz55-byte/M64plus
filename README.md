# M64plus

Emulador Nintendo 64 para Android. Plataforma inicial de desenvolvimento: Android 16, ARM64, com testes previstos no Poco C85 (Unisoc T7250 / Mali-G52 MC2).

## Estado atual

**Marco 0 — inicialização gráfica Android/OpenGL ES 2.0**

O aplicativo contém a estrutura Android mínima e um renderer de diagnóstico que desenha um triângulo colorido via `GLSurfaceView`. O renderer registra fabricante, GPU, versão OpenGL ES, extensões EGL e extensões GL importantes.

> O triângulo é apenas um teste do pipeline gráfico. Ainda não existe CPU MIPS, RSP, RDP, áudio, carregador de ROM ou emulação de jogos.

## Compilação

Abra a pasta raiz no Android Studio com JDK 17, Android Gradle Plugin 8.13.2 e Android SDK 36.

Também existe uma compilação automática pelo GitHub Actions em cada push ou pull request para `main`. O workflow executa `gradle assembleDebug`. O repositório ainda não inclui Gradle Wrapper.

## Teste no Poco C85

1. Instale e abra o APK de depuração gerado pelo workflow.
2. Confirme se o triângulo colorido aparece e permanece estável.
3. Conecte o aparelho por ADB e execute:

```bash
adb logcat -c
adb logcat -s M64plus-GPU:I
```

4. Envie as linhas `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, `IMPORTANT_GL_EXTENSIONS`, `EGL_EXTENSIONS` e `SURFACE_SIZE`.

## Próximas etapas

1. Validar a compilação automática e o teste gráfico no aparelho.
2. Escolher caminhos de renderização com base nas capacidades reais do driver Mali.
3. Criar o esqueleto nativo JNI e testes de memória/CPU.
4. Implementar a CPU MIPS/R4300 e o sistema de memória.
5. Implementar RSP e RDP com sincronização correta.
6. Integrar áudio, controles e carregamento de ROMs.

O repositório anterior `N64-Android` é independente e não é alterado por este projeto.
