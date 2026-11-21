plugins {
    id("com.android.application")
}
android {
    namespace = "com.jsr.m64plus"
    compileSdk = 36
    ndkVersion = "27.2.12479018"
    defaultConfig {
        applicationId = "com.jsr.m64plus"
        minSdk = 23
        targetSdk = 36
        versionCode = 2
        versionName = "0.2.0"
        externalNativeBuild {
            cmake { cppFlags += listOf("-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror") }
        }
    }
    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
