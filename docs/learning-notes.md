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
* **`.gitattributes`:** controla cómo trata git los archivos. `* text=auto eol=lf` normaliza saltos de línea; `*.torrent binary` evita que git altere los `.torrent` (contienen hashes SHA-1 en bruto). No imprescindible trabajando solo desde WSL. *(Detalle completo en la entrada del 2026-11-08.)*
* **Git no versiona carpetas vacías:** `include/` y `tests/` no aparecen en el remoto hasta que tengan un archivo (o un `.gitkeep`).
* **Idioma:** código, comentarios, commits y README en inglés (estándar de la industria, portfolio). La bitácora puede quedarse en español.
* **Comentarios:** explicar el *porqué* (decisiones no obvias), no el *qué* (eso está en la documentación y en esta bitácora).

---

## [2026-11-08] `.gitattributes`: saltos de línea y archivos binarios

### 1. Qué es
* `.gitignore` dice a git qué archivos **no guardar**; `.gitattributes` dice **cómo tratar** los que sí guarda. Va en la raíz del repo y se versiona como cualquier archivo.
* **Problema de fondo:** Linux termina las líneas con `\n` (LF) y Windows con `\r\n` (CRLF). Git puede convertir entre ambos; en texto es inofensivo, pero en un archivo binario corrompe los bytes.

### 2. Configuración del proyecto
```gitattributes
* text=auto eol=lf
*.torrent binary
```

| Línea | Afecta a | Qué hace |
|---|---|---|
| `* text=auto` | todos | detecta si es texto y, si lo es, lo guarda en el repo con LF |
| `eol=lf` | los de texto | los escribe en disco (checkout/clone) con LF |
| `*.torrent binary` | los `.torrent` | no se tocan: sin conversión, sin diff, sin merge |

* **`text=auto`:** git decide por archivo si es texto o binario (heurística: si hay un byte nulo `\0` al principio, es binario). Los de texto se **normalizan** a LF al hacer commit, aunque en disco tuvieran CRLF. Los binarios no se tocan.
* **`eol=lf`:** `text` controla cómo se **guarda** en el repo; `eol` controla cómo se **saca** al directorio de trabajo. Con `eol=lf`, los textos salen con LF en cualquier sistema, sin depender de `core.autocrlf` de cada máquina (p. ej. scripts `.sh` con CRLF no se ejecutan en Linux).
* **`binary`:** atajo (macro) de tres atributos:
  * `-text`: nunca convertir saltos de línea. Protege los hashes SHA-1 en bruto del `.torrent`: si uno de sus bytes es `0x0A` (`\n`) y git lo convierte, el archivo se corrompe y el parser falla sin motivo aparente.
  * `-diff`: `git diff` muestra solo `Binary files differ` en lugar de basura.
  * `-merge`: en un conflicto no se mezclan líneas (rompería el archivo); se elige una versión entera.

### 3. El orden importa
* Si varias líneas afectan al mismo archivo, **gana la última** (como en CSS). Primero la regla general (`*`), después las excepciones (`*.torrent`).
* Con el orden invertido, `* text=auto` (abajo) pisaba el `-text` de `binary` y el `.torrent` quedaba como `text: auto`: dependería de que la heurística acertase, que es justo lo que `binary` pretende evitar.

### 4. Verificación
```bash
git check-attr -a tests/data/debian-13.7.0-amd64-netinst.iso.torrent
```
Resultado correcto: `binary: set`, `diff: unset`, `merge: unset`, `text: unset`. El `eol: lf` que aparece es irrelevante: `eol` solo se aplica a archivos de texto.
* Salida vacía → el `.gitattributes` no existe, está vacío o no se ha guardado.

---

## [2026-11-08] Bencode, bytes y archivos binarios

### 1. Bencode: un formato *binary-safe*
* Formato de serialización de los `.torrent` (y de las respuestas del tracker y de DHT). Su **estructura** usa caracteres ASCII legibles:

| Tipo | Sintaxis | Ejemplo |
|---|---|---|
| Entero | `i<número>e` | `i42e` → 42 |
| String | `<longitud>:<bytes>` | `4:spam` → `spam` |
| Lista | `l<elementos>e` | `l4:spami42ee` → `["spam", 42]` |
| Diccionario | `d<clave><valor>...e` | `d3:keyi1ee` → `{"key": 1}` |

* **Clave del formato:** un string no se delimita por un carácter final, sino por su **longitud prefijada**. `26800:` significa "lee exactamente 26800 bytes, sean los que sean". Por eso el contenido puede ser cualquier byte (`\0`, `\n`, `0xFF`...) sin confundir al parser: el formato es *binary-safe*.

### 2. Por qué un `.torrent` es un archivo binario
* Casi todo se lee como texto (`d8:announce...`), salvo el campo `pieces`: la concatenación de los hashes SHA-1 de cada pieza (**20 bytes en bruto** cada uno, no en hexadecimal). Ej.: `6:pieces26800:` → 26800 / 20 = 1340 piezas.
* Los bytes de un hash son aleatorios a efectos prácticos: aparecen todos los valores posibles, incluidos `\0` y `\n`.
* **Lo que hace binario a un archivo es el formato, no su contenido concreto:** basta con que el formato *permita* bytes arbitrarios. Un `.torrent` cuyos hashes casualmente fueran todos imprimibles seguiría siendo binario.
* Otros formatos que mezclan texto y bytes: PDF (empieza por `%PDF-1.7` legible, luego datos comprimidos), PNG (`89` + `PNG`).

### 3. Bytes, ASCII y texto
* **Todos los archivos son bytes.** No existen "archivos de texto" como algo distinto en disco; la diferencia es cómo deben **interpretarse**:
  * **Texto:** *todos* los bytes son caracteres válidos en una codificación (ASCII, UTF-8...), organizados en líneas. Se puede abrir, editar y guardar sin perder nada.
  * **Binario:** *puede* contener bytes que no son texto, y cada byte cuenta exactamente como está.
* **ASCII es solo una tabla de traducción** número ↔ carácter (0-127). En disco, `d8:announce` son los bytes `64 38 3A 61 6E 6E ...`; un editor los "ve" como letras porque aplica esa tabla.
* En C++ un `char` **es** un byte y `'d'` **es** el número 100: comparar `byte == 'd'` funciona directamente, sin conversión.
* Rango imprimible ASCII: 32-126. Fuera de él (o en secuencias UTF-8 inválidas) no hay carácter que mostrar.

### 4. Cómo distingue un editor texto de binario
* **No hay ninguna marca universal: lo adivina** con heurísticas:
  1. **Bytes nulos (`0x00`):** casi nunca aparecen en texto. Git considera binario un archivo con `\0` en sus primeros 8000 bytes (base de `text=auto`).
  2. **Validez UTF-8:** UTF-8 impone reglas sobre qué bytes pueden ir juntos; si se incumplen a menudo, probablemente no es texto.
  3. **Extensión:** solo una pista (cualquiera puede renombrar un archivo).
* **Magic numbers (firmas):** muchos formatos binarios empiezan con bytes fijos, aunque no es universal:

| Formato | Primeros bytes | Como ASCII |
|---|---|---|
| PNG | `89 50 4E 47` | `.PNG` |
| PDF | `25 50 44 46` | `%PDF` |
| ZIP (`.docx`, `.jar`...) | `50 4B` | `PK` |
| Ejecutable Linux (ELF) | `7F 45 4C 46` | `.ELF` |

  * El comando `file` usa una base de datos de firmas. Bencode no tiene firma oficial, pero `file` reconoce un `.torrent` por su inicio `d8:announce`.
* **BOM (*Byte Order Mark*):** el equivalente para texto, `EF BB BF` en UTF-8. Opcional y desaconsejado en Linux (rompe scripts y compiladores).
* **Un byte no sabe si es texto:** solo es un número; es "texto" cuando alguien lo interpreta con una tabla de caracteres.

### 5. Qué hace VS Code al abrir un `.torrent`
* **Comprobación inicial:** busca bytes nulos al principio. Si los hay, no lo abre ("file is binary..."). El `.torrent` empieza con texto limpio y pasa el filtro.
* **Decodifica todo el archivo como UTF-8**, byte a byte, sin "zonas": lo válido se dibuja; lo inválido se sustituye por **`�`** (U+FFFD, carácter de sustitución). No hay cambio de modo: al principio todo es válido, en `pieces` casi nada.
* En `pieces` aparecen letras sueltas entre los `�` (p. ej. `PS^` en la guía): son bytes de hash que por casualidad caen en el rango imprimible.
* **Peligro al guardar:** al mostrar `�` el byte original ya se ha perdido en memoria. Guardar escribe `EF BF BD` (3 bytes) en su lugar → hashes destrozados y tamaño del archivo alterado. Recuperación: `git restore <archivo>`.

### 6. Cómo inspeccionar archivos binarios sin dañarlos
* **VS Code en solo lectura** (cómodo para el día a día), en `.vscode/settings.json`:
  ```json
  {
      "files.readonlyInclude": {
          "**/*.torrent": true
      }
  }
  ```
* **`less archivo`:** solo lectura; muestra los bytes no textuales escapados (`<9F>`, `^@`) en vez de `�`. `/texto` busca, `q` sale.
* **`strings archivo`:** extrae solo las secuencias de ≥4 caracteres imprimibles (URL del tracker, nombre de la ISO...).
* **`xxd archivo | less`** o **`hexdump -C archivo | less`:** offset + bytes en hexadecimal + columna ASCII (no imprimibles como `.`).
  * `xxd -s 300 -l 64 archivo`: 64 bytes desde la posición 300. Útil cuando el parser falla en un offset concreto.
* **Extensión Hex Editor** (Microsoft) en VS Code: *Open With… → Hex Editor*. Bytes y ASCII lado a lado, con el offset y el valor de cada byte. Usarla solo para mirar.
* **A futuro:** una función de *pretty-print* propia para el resultado del parser (árbol indentado, strings binarios como `<26800 bytes>` o en hex).

### 7. Implicaciones para el parser de bencode
* **No hay que "convertir a ASCII":** los bytes ya lo son. Se lee el archivo entero a un buffer de bytes y se recorre byte a byte, decidiendo por el carácter actual: `'i'` entero, `'l'` lista, `'d'` diccionario, dígito → string.
* **Abrir el archivo en modo binario:** en Linux no cambia nada, pero expresa la intención y es portable.
* **Nada de funciones de "C string"** (`strlen`, `strcpy`, `const char*` terminado en `\0`): se cortarían en el primer `\0` de `pieces`. Trabajar siempre con **puntero + longitud** (`std::string_view`). `std::string` también admite `\0` en medio.
* **La única conversión real son los números:** en `i351272960e` o `26800:` los dígitos son caracteres (`'2'` es el byte 50, no el 2). Herramienta adecuada: `std::from_chars` (C++17): sin excepciones, independiente del *locale* e indica hasta dónde ha leído.
* **Los strings no se convierten:** son "N bytes"; se guardan como vista (`std::string_view`) al buffer, sin copiar. Si son texto (`announce`, `name`) o binario (`pieces`) lo decide quien usa el resultado, no el parser.
* **Validar la longitud declarada:** si pone `26800:` y quedan 100 bytes, sin comprobarlo es un *buffer overflow* (el caso de manual para ASan).
* **No imprimir strings binarios** al depurar: mostrar su longitud o pasarlos a hexadecimal.

---

## [2026-11-08] Reestructuración: librería `btcore` y opciones compartidas

### 1. Estructura de archivos
```
include/btclient/          # headers públicos (#include "btclient/bencode.h")
├── bencode.h
└── bencode_printer.h
src/
├── bencode.cpp            # decoder: bytes → árbol de valores (no imprime nada)
├── bencode_printer.cpp    # printer: árbol → texto indentado (no sabe de bytes)
└── main.cpp               # el cliente
```
* **Separar *parsear* de *mostrar*:** el decoder se reutilizará para el `.torrent`, las respuestas del tracker y DHT; el printer solo sirve para depurar.
* **Subcarpeta `btclient/` en `include/`:** actúa como *namespace* de archivos. Evita choques de nombres con headers de terceros (`error.h`, `socket.h`...) y deja claro qué headers son del proyecto. Es la convención de todas las librerías serias (`<openssl/sha.h>`, `<boost/asio.hpp>`, `<fmt/core.h>`) y la que recomiendan la *Canonical Project Structure* (P1204) y el *Pitchfork layout*.

### 2. Los tres targets del `CMakeLists.txt`
```
btclient_options (INTERFACE)  ← warnings + -Werror + sanitizers
        │
        ├──► btcore (STATIC)  ← bencode.cpp, bencode_printer.cpp; expone include/
        │        │
        └──► bittorrent_client (ejecutable) ← main.cpp
```

### 3. `btclient_options`: librería `INTERFACE`
```cmake
add_library(btclient_options INTERFACE)
target_compile_options(btclient_options INTERFACE -Wall -Wextra ...)
```
* Una librería `INTERFACE` **no compila nada ni genera ningún archivo**: es un "paquete de configuración". Todo lo que se le añade con `INTERFACE` (flags de compilación, de enlazado, includes...) se aplica a quien la enlace.
* **Por qué:** con varios targets (librería, cliente y, más adelante, tests y herramientas), los warnings y sanitizers se definen **una sola vez** en lugar de copiar el bloque en cada target.
* **Por qué es crítico para los sanitizers:** ASan instrumenta los accesos a memoria **al compilar**. Si `btcore` se compilase sin `-fsanitize`, los accesos dentro del parser de bencode (el código que procesa datos no confiables) **no se comprobarían**, aunque el ejecutable sí lo tuviera.
* **Alternativas** que se ven en otros proyectos: una `function()` de CMake que aplica las flags a un target, o un `foreach` sobre la lista de targets. El target `INTERFACE` es la forma más idiomática en CMake moderno, porque usa el mismo mecanismo de propagación que cualquier dependencia.

### 4. `btcore`: librería estática
```cmake
add_library(btcore STATIC src/bencode.cpp src/bencode_printer.cpp)
target_include_directories(btcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(btcore PRIVATE btclient_options)
```
* **`STATIC`:** genera `libbtcore.a`, un archivo con los `.o` empaquetados (con `ar`). Al enlazar el ejecutable, el código necesario se copia dentro del binario; no hace falta distribuir la librería aparte (a diferencia de una `.so` dinámica).
* **`PUBLIC` en los includes:** `btcore` usa `include/` para compilarse **y** quien enlace `btcore` lo recibe automáticamente. Por eso el ejecutable ya no tiene su propio `target_include_directories`: puede hacer `#include "btclient/bencode.h"` solo por enlazar `btcore`.
* **`PRIVATE` con `btclient_options`:** las flags se aplican al compilar `btcore`, pero `btcore` no "obliga" a sus consumidores a usarlas. Cada target decide enlazar `btclient_options` por su cuenta.
* **Por qué una librería:** el ejecutable, los tests y las herramientas (`bencode_dump`) enlazarán el mismo código sin compilarlo ni duplicarlo varias veces.

### 5. `bittorrent_client`: el ejecutable
```cmake
add_executable(bittorrent_client src/main.cpp)
target_link_libraries(bittorrent_client PRIVATE btcore btclient_options)
```
* Solo contiene `main.cpp`; toda la lógica vive en `btcore`.
* Enlaza `btclient_options` explícitamente porque, al ser `PRIVATE` en `btcore`, sus flags no llegan por esa vía.
* Las opciones de enlazado de los sanitizers (`-fsanitize=...` en el *link*) se aplican aquí, que es donde se genera el binario final y se enlaza el runtime de ASan/UBSan.

### 6. `PRIVATE`, `PUBLIC` e `INTERFACE` en `target_link_libraries`
| Palabra | ¿Se aplica a este target? | ¿Se propaga a quien lo enlace? |
|---|---|---|
| `PRIVATE` | sí | no |
| `PUBLIC` | sí | sí |
| `INTERFACE` | no | sí |

* Las mismas reglas valen para `target_include_directories`, `target_compile_options`, etc.

### 7. Cómo crecer a partir de aquí
* **Nuevo `.cpp` de lógica** (p. ej. `src/torrent.cpp`): añadirlo a la lista de `add_library(btcore ...)`.
* **Nuevo ejecutable** (tests, `bencode_dump`):
  ```cmake
  add_executable(bencode_dump tools/bencode_dump.cpp)
  target_link_libraries(bencode_dump PRIVATE btcore btclient_options)
  ```
* **Verificación:** `cmake --build build --verbose` debe mostrar `-fsanitize=address,undefined` en la compilación de **todos** los `.cpp` y en el enlazado final.
* Una librería estática con `.cpp` vacíos compila sin problemas (en C++ una unidad de traducción vacía es válida; en C, `-Wpedantic` avisaría).

### 8. Convenciones de headers y `namespace`
* **`#pragma once`:** evita que un header se procese dos veces en la misma unidad de traducción. No es estándar ISO, pero lo soportan GCC, Clang y MSVC. Frente a los *include guards* clásicos (`#ifndef BTCLIENT_BENCODE_H` / `#define` / `#endif`): más corto y sin el error típico de copiar un `.h` y olvidar renombrar el guard.
* **Cada `.cpp` incluye primero su propio header:** comprueba que el header es autosuficiente. Si le falta un `#include`, falla al compilar su `.cpp` en lugar de funcionar por casualidad en otro sitio donde ese include ya estaba.
* **`namespace`:** es a los **nombres del código** (tipos, funciones, variables) lo que la carpeta `include/btclient/` es a los **nombres de archivo**: un "apellido" que evita choques.
  * Problema: nombres genéricos como `Value` o `decode` en el ámbito global pueden chocar con los de una librería externa → error de redefinición o, peor, se usa el que no es.
  * Uso: las declaraciones del `.h` **y** las definiciones del `.cpp` van dentro del mismo `namespace btclient { ... }`. Desde fuera: `btclient::decode(...)`.
  * Ya se usaba sin saberlo: la librería estándar vive en `std` (`std::string`, `std::cout`...).
  * Convención: comentar la llave de cierre, `}  // namespace btclient`, porque el bloque suele ocupar todo el archivo.
  * **Nunca `using namespace` en un `.h`:** se aplicaría a todo archivo que lo incluya y reintroduce los choques. En un `.cpp` es aceptable (aunque escribir siempre `std::` es lo más claro).
  * Un namespace por proyecto suele bastar; si crece, se pueden anidar con la sintaxis de C++17: `namespace btclient::bencode { ... }`.