# Bitácora de Aprendizaje

Notas técnicas, decisiones de diseño y conceptos de bajo nivel aprendidos durante el desarrollo del cliente BitTorrent en C++20.

---

## [2026-10-30] Configuración inicial, Toolchain y vistas sin copia

### 1. Arquitectura VS Code + WSL2
* **Concepto:** Arquitectura cliente-servidor desacoplada. El frontend corre en Windows (interfaz gráfica Electron), mientras que el backend (`vscode-server`) se ejecuta dentro de Linux ext4.
* **Comunicación:** Se comunican mediante sockets locales / IPC (`vsock`). El Language Server Protocol (LSP) y GDB corren de forma nativa en Linux, lo que evita penalizaciones de I/O en llamadas al sistema o accesos cruzados mediante el protocolo 9P (`/mnt/c/`).

### 2. CMake Moderno y Flags Estrictos
* **Targets (`add_executable`, `target_include_directories`):** En CMake moderno se configuran propiedades por target en lugar de usar comandos globales. `PRIVATE` asegura que las rutas de cabeceras pertenezcan solo a ese binario.
* **`CMAKE_EXPORT_COMPILE_COMMANDS`:** Genera `compile_commands.json` en `build/` para que `clangd` / LSP resuelvan tipos e includes en tiempo real con cero falsos positivos. *(Eliminado el 2026-11-07, ver sección 5 de esa entrada.)*
* **Flags clave:**
  * `-Wall -Wextra`: Avisos de código sospechoso (variables sin usar o sin inicializar, parámetros sin usar, caminos sin `return`, *fallthrough* en `switch`). `-Wall` no activa "todos" los avisos pese al nombre.
  * `-Wpedantic`: Avisa de cualquier uso no estándar de C++ (extensiones del compilador).
  * `-Wconversion`: Detecta conversiones implícitas que pueden perder datos (p. ej. `uint64_t` → `uint32_t`). En C++ con GCC **no** cubre el cambio de signo (ver `-Wsign-conversion`, 2026-11-07).
  * `-Werror`: Trata cualquier advertencia como fallo de compilación para forzar código limpio en cada commit.

### 3. Vistas de memoria: `std::string_view` (C++17)
* **Qué es en memoria:** Un struct de solo 16 bytes (en 64 bits) compuesto por dos valores: un puntero de lectura (`const char* ptr`) y un tamaño (`size_t len`).
* **Por qué importa:** A diferencia de `std::string`, no asigna memoria dinámica en el heap (`malloc`) ni copia caracteres. Permite inspeccionar y trocear cadenas en $O(1)$ sin sobrecarga.
* **Invariante crítica:** No es dueño de los datos. El buffer o secuencia de memoria original al que apunta debe seguir viva mientras la vista esté en uso (riesgo de *dangling pointer* si el contenedor original se destruye).

---

## [2026-11-07] Sanitizers, tipos de build y opciones de CMake

### 1. Sanitizers: ASan y UBSan
* **Concepto:** Instrumentación que el compilador (GCC/Clang) inserta en el binario. Cada acceso a memoria u operación sospechosa se comprueba en tiempo de ejecución; ante una violación, el programa aborta indicando el tipo de error, el archivo y la línea, con un stack trace.
* **Problema que resuelven:** En C++ muchos errores son *undefined behavior* (UB): no fallan en el momento, sino más tarde, en otro sitio, de forma intermitente o solo en otra máquina. Los sanitizers convierten ese UB silencioso en un fallo inmediato y localizado.
* **ASan (AddressSanitizer):** Errores de memoria.
  * *Buffer overflow* (lectura/escritura fuera de los límites, en heap o en stack).
  * *Use-after-free* y *double free*.
  * Fugas de memoria al salir (en Linux incluye LeakSanitizer).
* **UBSan (UndefinedBehaviorSanitizer):** Otros tipos de UB.
  * Overflow de enteros con signo.
  * Shifts inválidos (p. ej. `uint8_t << 24` promocionado a `int` y desbordado).
  * Accesos desalineados, desreferencia de `nullptr`, división entre cero.
* **Coste:** ~2x más lento y bastante más memoria → solo en `Debug`.
* **Por qué es crítico aquí:** El cliente parsea datos binarios no confiables que llegan de la red (`.torrent`, respuestas del tracker, mensajes de peers). Un prefijo de longitud que miente ("mido 1000 bytes" y llegan 10) sin validar es un *buffer overflow*, el origen clásico de vulnerabilidades.
* **En CMake:**
  ```cmake
  set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_compile_options(bittorrent_client PRIVATE "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>")
  target_link_options(bittorrent_client PRIVATE "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>")
  ```
  * Las flags van tanto al compilar como al enlazar (el runtime del sanitizer se enlaza con el binario). Solo al compilar → error de enlazado `undefined reference to __asan_...`.
  * `-fno-omit-frame-pointer`: stack traces completos y legibles.
  * `$<$<CONFIG:Debug>:...>`: *generator expression*; solo aplica las flags en builds `Debug`.
  * Entre comillas porque `SANITIZER_FLAGS` es una lista (`a;b`): sin comillas CMake partiría la expresión en dos argumentos por el `;`. Funciona igualmente, pero las comillas la mantienen como una sola expresión.
  * Se usa *generator expression* y no `if(CMAKE_BUILD_TYPE STREQUAL "Debug")` porque el `if` se evalúa al leer el archivo, y en generadores multi-configuración (Visual Studio, Ninja Multi-Config) la configuración aún no se conoce. La *genex* se evalúa al generar/compilar, así que funciona siempre.
  * ASan es incompatible con Valgrind: para usar Valgrind, `-DENABLE_SANITIZERS=OFF`.
* **Cómo leer un informe:** la traza se lee de arriba abajo; `#0` es donde ocurrió el error y las siguientes líneas son quién la llamó.

### 2. Tipos de build (`CMAKE_BUILD_TYPE`)
| Tipo | Flags GCC | Uso |
|---|---|---|
| `Debug` | `-g` | Depurar |
| `Release` | `-O3 -DNDEBUG` | Versión final (desactiva `assert`) |
| `RelWithDebInfo` | `-O2 -g -DNDEBUG` | Perfilado |
| *(vacío)* | ninguna | Ni depurable ni optimizado |

* Si no se indica, queda vacío. Se fuerza `Debug` por defecto solo cuando el usuario no ha elegido otro y el generador es de configuración única (Make/Ninja):
  ```cmake
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
      set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()
  ```
* `FORCE` es necesario porque CMake ya crea la variable vacía en la caché; el `if` garantiza que nunca pisa una elección explícita.
* Una carpeta de build por tipo: `cmake -B build-release -DCMAKE_BUILD_TYPE=Release` (añadir `build-*/` al `.gitignore` si se usa).

### 3. `option()` y la caché de CMake
* `option(NOMBRE "descripción" ON)` crea un interruptor booleano modificable desde fuera: `cmake -B build -DNOMBRE=OFF`.
* **Trampa:** Las opciones se guardan en `build/CMakeCache.txt`. Cambiar el valor por defecto en el `CMakeLists.txt` **no afecta** a un `build/` ya configurado → pasar `-D` o borrar `build/`.
* Opciones del proyecto: `WARNINGS_AS_ERRORS` y `ENABLE_SANITIZERS` (ambas `ON` por defecto).

### 4. Estándar de C++
* `CMAKE_CXX_STANDARD 20` → `-std=c++20`.
* `CMAKE_CXX_STANDARD_REQUIRED ON` → si el compilador no soporta C++20, falla al configurar en vez de bajar en silencio a C++17.
* `CMAKE_CXX_EXTENSIONS OFF` → `-std=c++20` en lugar de `-std=gnu++20`; prohíbe extensiones de GCC (p. ej. VLAs `int buf[n];`). Junto con `-Wpedantic`, garantiza C++ estándar portable.
* Alternativa por target: `target_compile_features(bittorrent_client PUBLIC cxx_std_20)`. Para un proyecto pequeño, el `set` global es suficiente.
* Utilidades de C++20 relevantes para el proyecto: `std::span` (vista de bytes sin copia, el equivalente de `string_view` para buffers binarios) y `std::endian` (el protocolo usa enteros big-endian). `std::format` requiere GCC 13+.

### 5. Cambios respecto a la configuración del 2026-10-30
* `-Werror` pasa a ser opcional (`WARNINGS_AS_ERRORS`): con `-Wconversion`, manipular bytes genera muchos avisos y conviene poder desactivarlo puntualmente.
* Añadido `-Wsign-conversion`: en GCC, `-Wconversion` no cubre del todo las mezclas `int`/`size_t`, el error clásico al indexar buffers.
* Eliminado `CMAKE_EXPORT_COMPILE_COMMANDS`: solo lo necesita clangd, y se usa la extensión C/C++ de Microsoft (IntelliSense propio).
* Ejecutable renombrado a `bittorrent_client` (igual que el proyecto).
* Ruta de includes absoluta: `${CMAKE_CURRENT_SOURCE_DIR}/include`.

### 6. Warnings: lo que añade cada flag nueva
* `-Wsign-conversion`: Avisa al convertir entre con signo y sin signo **en ambas direcciones**. Caso típico: `int i = -1; size_t pos = i;` → `pos` vale `18446744073709551615`. Crítico al indexar buffers (`size_t`) con cálculos hechos en `int`.
* `-Wshadow`: Avisa cuando una variable tapa a otra con el mismo nombre (p. ej. parámetro `port` que oculta el miembro `port`).
* `-Werror` condicional mediante `option(WARNINGS_AS_ERRORS ...)`: impide que los warnings se acumulen, pero se puede desactivar mientras se experimenta.

### 7. `cmake_minimum_required` y `project()`
* `cmake_minimum_required(VERSION 3.20)`: además de exigir esa versión mínima, fija las *policies* (el comportamiento de CMake) al de la 3.20, aunque esté instalada una más nueva → mismo resultado en cualquier máquina.
* `project(nombre VERSION x.y.z LANGUAGES CXX)`:
  * El nombre del proyecto **no** es el del ejecutable (ese lo decide `add_executable`).
  * `VERSION` rellena `PROJECT_VERSION`, `PROJECT_VERSION_MAJOR`, etc.; útil para un `--version` o para el *peer id* de BitTorrent (p. ej. `-QB4520-` = qBittorrent 4.5.2).
  * `LANGUAGES CXX`: sin él CMake asume C y C++ y busca también un compilador de C innecesario.

### 8. Versionado (SemVer)
* Formato **MAJOR.MINOR.PATCH**. Se actualiza a mano en `project()`; CMake nunca la cambia.
  * PATCH: arreglo de bugs. MINOR: nueva funcionalidad (hito). MAJOR: estable / cambio incompatible.
  * En `0.x` todo puede cambiar: se sube MINOR al cumplir hitos (parsear `.torrent`, hablar con el tracker, descargar un archivo...).
* No se sube en cada commit (para eso está git). Al subirla, se marca el commit: `git tag v0.2.0` + `git push --tags`.

### 9. `add_executable` y `target_include_directories`
* En `add_executable` se listan los `.cpp` explícitamente; los `.h` se encuentran vía `#include`. Evitar `file(GLOB ...)`: CMake no detecta archivos nuevos sin reconfigurar.
* `target_include_directories` se traduce en `-I<ruta>`: permite `#include "bencode.h"` en vez de rutas relativas.
* Visibilidad:
  * `PRIVATE`: solo este target.
  * `PUBLIC`: este target y los que lo enlacen.
  * `INTERFACE`: solo los que lo enlacen.
  * En un ejecutable da igual (nadie lo enlaza). Importará al separar la lógica en una librería: sus headers serán `PUBLIC` para que el ejecutable y los tests los vean.
* `${CMAKE_CURRENT_SOURCE_DIR}`: carpeta del `CMakeLists.txt` que se está procesando.

### 10. Repositorio y convenciones
* **`.gitignore` mínimo:** `build/` (CMake genera todo dentro: objetos, `CMakeCache.txt`, `compile_commands.json`, `Testing/`...) y `*:Zone.Identifier` (metadatos que crea Windows al copiar archivos a WSL). Se añaden reglas solo cuando aparece algo real en `git status`.
* **`.gitattributes`:** controla cómo trata git los archivos. `* text=auto eol=lf` normaliza saltos de línea; `*.torrent binary` evita que git altere los `.torrent` (contienen hashes SHA-1 en bruto). No imprescindible trabajando solo desde WSL.
* **Git no versiona carpetas vacías:** `include/` y `tests/` no aparecen en el remoto hasta que tengan un archivo (o un `.gitkeep`).
* **Idioma:** código, comentarios, commits y README en inglés (estándar de la industria, portfolio). La bitácora puede quedarse en español.
* **Comentarios:** explicar el *porqué* (decisiones no obvias), no el *qué* (eso está en la documentación y en esta bitácora).