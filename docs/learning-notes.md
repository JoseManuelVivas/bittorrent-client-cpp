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

### 9. Includes: la ruta empieza *dentro* de `include/`
* `target_include_directories(btcore PUBLIC .../include)` → el compilador busca **dentro** de `include/`. La ruta del `#include` se pega detrás:
  * `#include "include/btclient/bencode.h"` → busca `.../include/include/btclient/bencode.h` ✗
  * `#include "btclient/bencode.h"` → `.../include/btclient/bencode.h` ✓
* **VS Code no lo marcó como error:** la extensión C/C++ de Microsoft busca por defecto en todo el proyecto (`${workspaceFolder}/**`) y encuentra el archivo con cualquier ruta razonable; el compilador solo busca donde dice CMake. Para que el editor vea lo mismo que el compilador: extensión **CMake Tools** como proveedor de configuración (`"C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"`).

---

## [2026-11-08] Diseño del tipo `Value`: `std::variant` y tipos recursivos

### 1. Qué es un "valor bencode"
* Cualquiera de los cuatro tipos: entero, string, lista o diccionario. Las listas y diccionarios **contienen otros valores** → estructura **recursiva** (un árbol).
* **El fichero entero es un único valor** (un diccionario), y cada nodo de dentro también lo es:
  ```
  diccionario                                  ← el fichero entero (1 valor)
  ├── "announce"      → string
  ├── "creation date" → entero
  ├── "url-list"      → lista
  │                     ├── string
  │                     └── string
  └── "info"          → diccionario
                        ├── "length"       → entero
                        ├── "name"         → string
                        ├── "piece length" → entero
                        └── "pieces"       → string (binario)
  ```
* El tipo `Value` no representa "el fichero": representa **cualquier nodo** del árbol. El decoder devuelve **un** `Value`, la raíz.
* Las claves de un diccionario bencode son siempre strings; solo los valores son recursivos.

### 2. Tipo suma (*tagged union*)
* `Value` no es "un diccionario de algo genérico": es un tipo que es **exactamente una** de varias alternativas en cada momento:
  ```
  Value = entero | string | lista de Value | diccionario de (string → Value)
  ```
* No es "genérico" en el sentido de plantillas (`template<T>`); es un **tipo suma** o *tagged union*. Otros lenguajes lo traen de serie (los `enum` de Rust).

### 3. `union` vs `std::variant`
* **`union` (C/C++ clásico):** reserva memoria para el miembro más grande y todos comparten esos bytes. Problemas:
  * **No sabe qué contiene:** hay que llevar la cuenta a mano con un *tag*. Leer el miembro equivocado es *undefined behavior*.
  * **No gestiona constructores ni destructores:** con `std::string`, `std::vector` o `std::map` habría que usar *placement new* y llamar a los destructores a mano → fugas, *double free*.
* **`std::variant` (C++17, `<variant>`):** *tagged union* segura.
  * Sabe en todo momento qué alternativa contiene.
  * Construye y destruye correctamente lo que lleva dentro.
  * Leer la alternativa equivocada se detecta (excepción o puntero nulo) en vez de devolver basura.

### 4. API básica de `std::variant`
| Función | Qué hace |
|---|---|
| `v.data = 42;` | asigna; `variant` elige la alternativa que encaja |
| `std::holds_alternative<T>(v.data)` | `true` si contiene la alternativa `T` |
| `std::get<T>(v.data)` | devuelve la alternativa `T`; si no es esa, lanza `std::bad_variant_access` |
| `std::get_if<T>(&v.data)` | puntero a la alternativa `T`, o `nullptr` si no es esa (sin excepciones) |
| `std::visit(f, v.data)` | ejecuta `f` con la alternativa que haya; ideal para el printer |

* El tipo entre `< >` debe ser el **exacto** de la alternativa (si se declaró `std::int64_t`, se pregunta por `std::int64_t`).
* Experimento: pedir `std::get<std::string>` a un `Value` con un entero → termina con `std::bad_variant_access`. Esa es la seguridad que una `union` no da.

### 5. Sintaxis y nombres
* Una variable se declara `TIPO nombre;`, y los `< >` forman parte del **tipo**:
  * ✗ `std::variant test<int, std::string>;`
  * ✓ `std::variant<int, std::string> data;`
* **Tipo vs miembro:** el `struct` es el tipo (`Value`, en `PascalCase`); el miembro es la variable de dentro (`data`, en `snake_case`). Evitar `value.value`: `v.data` se lee mejor.
* **Enteros de 64 bits:** `std::int64_t` (`<cstdint>`). Un `int` de 32 bits (máx. ~2.100 millones) no basta para el `length` de un torrent de varios GB.
* **Incluir siempre lo que se usa:** `<string>` aunque `<variant>` lo incluya por dentro en GCC; no está garantizado.

### 6. Tipos incompletos y recursividad
* **Un tipo no puede contenerse a sí mismo por valor:** su tamaño sería infinito (`Caja` = entero + `Caja` = entero + entero + `Caja`...).
* **Incomplete type:** mientras el compilador lee la definición de un tipo y no ha llegado al final, sabe que existe pero no cuánto ocupa. Con él se pueden tener **punteros y referencias** (tamaño fijo, 8 bytes en 64 bits), pero no objetos por valor.
* **Salida: indirección.** Un `std::vector` no guarda sus elementos dentro de sí: ocupa siempre lo mismo (puntero al heap + tamaño + capacidad). Por eso un `Value` **sí** puede contener un `std::vector<Value>`.
* **Alias vs `struct`:** `using Value = std::variant<..., std::vector<Value>>;` no compila: un alias no existe hasta que termina su línea. Dentro de las llaves de un `struct`, en cambio, su nombre ya existe (aunque incompleto). Por eso `Value` es un `struct` que envuelve el `variant`.
* **Declaración adelantada** (`struct Value;`): anuncia el nombre antes de definirlo. Dentro del propio `struct` no hace falta; sí cuando otro código necesita nombrar el tipo antes de su definición.
* **Patrón** (ejemplo análogo, sistema de archivos):
  ```cpp
  struct Entry;
  struct Entry {
      std::variant<std::string, std::vector<Entry>> content;
  };
  ```
* **Contenedores y tipos incompletos:**
  * `std::vector`, `std::list`, `std::forward_list`: **garantizado** desde C++17.
  * `std::map`: **no garantizado** por el estándar (funciona con libstdc++/GCC en la práctica).

### 7. Decisión pendiente: representación del diccionario
| Opción | A favor | En contra |
|---|---|---|
| `std::map<std::string, Value>` | natural; claves ordenadas automáticamente; búsqueda rápida | el estándar no garantiza que funcione con tipo incompleto |
| `std::vector<std::pair<std::string, Value>>` | garantizado; conserva el orden del fichero (en bencode válido ya viene ordenado) | buscar una clave es recorrerlo (irrelevante con 5-10 claves) |
* **Info hash (a futuro):** es el SHA-1 del diccionario `info` **exactamente como venía en el fichero**. Cualquier alteración (orden de claves, número reescrito) cambia el hash. Idea: que el decoder recuerde en qué posición del buffer empezaba y terminaba `info`, en lugar de re-codificarlo como hace la guía de Go.

### 8. Usar el tipo
* Acceso sin copiar: `const auto& elementos = std::get<std::vector<Value>>(raiz.data);`. `const auto&` es una **referencia constante**: accede al vector dentro de `raiz` sin copiarlo (sin `&` se copiaría el vector entero).
* Recorrido con `for` de rango: `for (const auto& elem : elementos) { ... }`.
* `std::boolalpha` en el `cout` imprime `true`/`false` en vez de `1`/`0`.

### 9. Errores encontrados por el camino
* **`;` antes del `else`:** toda sentencia acaba en `;`, también en un `if` sin llaves. Usar **llaves siempre** en ambas ramas evita estos despistes.
* **Parámetros sin usar** (`argc`, `argv` al comentar su uso): `-Wextra` avisa y `-Werror` lo convierte en error. Soluciones: `int main()` o el atributo `[[maybe_unused]]` delante de cada parámetro.
* **`};` al cerrar un `namespace`:** sobra. `struct` y `class` llevan `;` tras la llave; `namespace` no. `-Wpedantic` lo detecta como "extra ';'".

### 10. CMake: configurar y compilar
* **Configurar** (`cmake -B build`): lee el `CMakeLists.txt`, detecta el compilador y **genera** en `build/` los archivos de la herramienta real (en Linux, `Makefile`s). No compila.
* **Compilar** (`cmake --build build`): ejecuta esa herramienta (equivale a `cd build && make`). `--build` es la opción ("compila") y `build` su argumento (la carpeta; el nombre coincide por convención).
* **Incremental:** solo recompila lo que ha cambiado (y los `.cpp` que incluyen un header modificado).
* Si cambia el `CMakeLists.txt`, `cmake --build` reconfigura solo. Reconfigurar a mano solo al borrar `build/` o cambiar opciones (`-D...`).

| Comando | Qué hace |
|---|---|
| `cmake --build build --verbose` | muestra el comando `g++` exacto |
| `cmake --build build -j` | compila en paralelo |
| `cmake --build build --target btcore` | solo ese target |
| `cmake --build build --clean-first` | recompila todo desde cero |

### 11. Estado y siguientes pasos
1. ✅ `variant` con entero (`std::int64_t`) y string; probado en `main.cpp`.
2. ✅ Añadir la lista (`std::vector<Value>`) y probarla: crear `[42, "hola"]`, recorrerla e imprimir el tipo de cada elemento.
3. ✅ Añadir el diccionario (decidir la representación, sección 7). *(Ver entrada siguiente.)*
4. ⬜ Empezar el decoder.

---

## [2026-11-08] Diccionario, `find()`, propiedad de la memoria y `string_view`

### 1. Representación del diccionario
* **Decisión:** `std::vector<std::pair<std::string, Value>>` en lugar de `std::map` (garantizado con tipos incompletos y conserva el orden del fichero; ver sección 7 de la entrada anterior).
* **`std::pair` mejor que `std::tuple`** (`<utility>`): para dos elementos es lo idiomático; `.first` (clave) y `.second` (valor) se leen mejor que `std::get<0>`/`std::get<1>`. Los elementos de un `std::map` también son `std::pair`. `std::tuple` es para tres o más elementos.
* **Alias con `using`** (`List`, `Dict`): evitan repetir tipos largos en cada `std::get<...>` / `holds_alternative<...>`.
* **Aquí sí hace falta la declaración adelantada:** los alias mencionan `Value` y deben declararse **antes** del `struct` para usarlos dentro del `variant`. `struct Value;` anuncia el nombre; los alias se definen con el tipo aún incompleto (el vector lo permite); después se define el `struct`.
  ```cpp
  struct Value;
  using List = std::vector<Value>;
  using Dict = std::vector<std::pair<std::string, Value>>;
  struct Value { std::variant<std::int64_t, std::string, List, Dict> data; };
  ```
* **Nombres:** `List`/`Dict` en `PascalCase` como `Value` (coherencia). `list` en minúscula además se confunde con `std::list`.
* **IntelliSense vs compilador:** el subrayado rojo de VS Code viene del analizador propio de la extensión C/C++, que con tipos incompletos en plantillas a veces se queja de código que GCC acepta. **Manda el compilador:** si `cmake --build build` compila, el código es válido.
* **Pendiente para el decoder:** la especificación exige claves **únicas y ordenadas**. Un vector no lo impide; se valida al decodificar.

### 2. Diseño de `find()`
```cpp
[[nodiscard]] const Value* find(const Value& dictionary, std::string_view key);
```
* **Parámetro por referencia, retorno por puntero:**
  * Referencia → argumento **obligatorio** (no puede ser nula; no hay que comprobar `nullptr`).
  * Puntero → "quizá no hay nada" es un resultado válido (`nullptr` si no existe la clave o si `dictionary` no es un `Dict`).
* **Orden:** primero el objeto sobre el que se opera, luego lo que se busca (`find(dictionary, key)`).
* **Nombre:** `find` es la convención de la librería estándar para "busca y dime dónde está o nada" (`std::find`, `std::map::find`, `std::string::find`). `std::search` es otra cosa (buscar una *secuencia* dentro de otra).
* **Clave como `std::string_view`:** permite llamar con `"announce"`, con un `std::string` o con un trozo de buffer sin crear copias. `std::string == std::string_view` se compara directamente.
* **Comprobar el tipo sin excepciones:** `std::get_if<Dict>(&dictionary.data)` devuelve un puntero al `Dict` o `nullptr`.
* **"No encontrado" es un caso normal**, no un error: en un `.torrent` hay claves obligatorias (`info`) y opcionales (`comment`).
* **Trampa:** en el `for` de rango, `auto` sin `&` copia cada elemento; devolver la dirección de esa copia deja un *dangling pointer* (ASan: *stack-use-after-scope*). Usar `const auto&`.
* **Declaración en `bencode.h`, definición en `bencode.cpp`**, ambas dentro de `namespace btclient`. Declarada pero sin definir compila mientras nadie la llame; si se usa → error de enlazado *undefined reference to `btclient::find`*.

### 3. Propiedad de la memoria: ¿smart pointer o puntero normal?
* **Los smart pointers expresan propiedad** (quién libera la memoria). `find` no transfiere propiedad: el dueño de cada `Value` es el vector que lo contiene; `find` solo "señala".
  * `std::unique_ptr` → "soy el único dueño y libero al destruirme". Liberaría un elemento que sigue perteneciendo al vector → **double free**.
  * `std::shared_ptr` → propiedad compartida con contador. Obligaría a guardar `shared_ptr<Value>` en el diccionario: rediseño, contador atómico y una reserva de memoria por nodo, solo para buscar.
* **Convención (C++ Core Guidelines):** smart pointer = **posee**; puntero normal `T*` = **observa sin poseer** (y puede ser `nullptr`). Mismo patrón que los iteradores de `std::map::find`.
* **La vida útil se garantiza por diseño:** el árbol de `Value`s debe vivir más que los punteros obtenidos de él (natural: se decodifica una vez y luego se consulta). Si se rompe la regla, ASan avisa (`heap-use-after-free`).
* **Invalidación por modificación:** si se añade un elemento a un `std::vector` sin capacidad libre, **realoja** todos sus elementos y los punteros/referencias a ellos quedan colgando aunque el vector siga vivo. Por eso el contrato de `find` dice *"while it is alive and unmodified"*.

### 4. `std::string_view` en profundidad
* **Analogía:** `std::string` es **tener el libro** (tuyo; si lo tiras, desaparece). `std::string_view` es **un marcapáginas con nota** ("libro de Juan, página 40, líneas 3-7"): barato, pero si Juan tira el libro, apunta a nada.
* **En memoria:** `{ puntero al primer carácter, longitud }` → 16 bytes, sin reservar memoria ni copiar.
  ```
  std::string s = "announce";      // s es dueño de los caracteres (heap)
  std::string_view v1 = s;         // { ptr→'a', 8 }
  std::string_view v2 = v1.substr(4); // { ptr→'u', 4 } = "unce", sin copiar
  ```
  `substr` en un `string_view` solo mueve el puntero y la longitud; en un `std::string` crea una copia nueva.
* **Usos en el proyecto:**
  * **Parámetros de solo lectura:** acepta literales, `std::string` y trozos de buffer sin copias. Con `const std::string&`, llamar con `"announce"` crearía un `std::string` temporal (y una reserva de memoria).
  * **Parser:** cada string bencode ya está en el buffer del fichero; la vista lo señala sin copiarlo.
* **Trampas:**
  1. **Dangling:** que el dueño muera antes que la vista (devolver una vista a un `std::string` local, o crearla desde un temporal). ASan: `stack-use-after-return` / `heap-use-after-free`. **Regla:** una vista nunca debe vivir más que el texto al que apunta. Como parámetro es seguro (quien llama mantiene el texto vivo durante la llamada); guardarla o devolverla exige pensar quién es el dueño.
  2. **No termina en `\0`:** no pasar `v.data()` a funciones de C (`printf("%s")`, `fopen`). `std::cout << v` sí respeta la longitud.

| | `std::string` | `std::string_view` |
|---|---|---|
| ¿Dueño del texto? | sí | no, solo lo señala |
| ¿Copia al crearse? | sí | no |
| Tamaño | variable (reserva memoria) | 16 bytes fijos |
| ¿Modificable? | sí | no (solo lectura) |
| Riesgo | ninguno de vida útil | quedar colgando si muere el dueño |

### 5. Literales de texto: dónde viven
* `"announce"` escrito en el código lo mete el compilador **dentro del ejecutable**, en la sección de solo lectura **`.rodata`**. Al ejecutar, se carga en memoria con el programa y vive **durante toda la ejecución**.
* `find(dict, "announce")` recibe `{ puntero → 'a' en .rodata, 8 }`: datos reales que ya estaban en memoria.
* Comprobación: `strings build/bittorrent_client | grep announce`.
* **Una vista a un literal nunca queda colgando.** El caso peligroso es el texto creado en tiempo de ejecución por un `std::string` que se destruye.

### 6. Paso por valor y zonas de memoria
* **Por valor = copia en el parámetro**, una variable local en la pila que se destruye al salir. Lo que importa es **qué** se copia:
  * `std::string_view` por valor → se copian los 16 bytes de la vista, **no el texto**. Por eso `string_view` se pasa **siempre por valor** (nunca `const std::string_view&`).
  * `std::string` por valor → se crea otro string que **reserva memoria y copia todos los caracteres** (con `pieces`, 26800 bytes). Para solo leer: `const std::string&` o `std::string_view`.
* Regla: copiar un objeto **vista** no copia los datos; copiar un objeto **dueño** (`string`, `vector`) copia también sus datos.

| Zona | Qué guarda | Cuánto vive |
|---|---|---|
| `.rodata` (estática) | literales como `"announce"` | todo el programa |
| Pila (*stack*) | variables locales y parámetros (incluidos los copiados por valor) | hasta el final de la función o bloque `{ }` |
| Heap | el contenido que reservan `std::string`, `std::vector`... | hasta que su dueño lo libera |

* Un `std::string` local es mixto: el objeto (puntero, tamaño, capacidad) en la pila; **sus caracteres** en el heap. Al destruirse el objeto libera los caracteres → una vista a ellos queda colgando.

### 7. `[[nodiscard]]`
* Atributo (C++17), como `[[maybe_unused]]`: "el valor devuelto es importante; si se ignora, avisa". Con `-Werror` → error de compilación.
  ```cpp
  find(dict, "announce");                   // ⚠ ignoring return value
  const Value* v = find(dict, "announce");  // ✓
  if (find(dict, "announce")) { ... }       // ✓
  ```
* **Cuándo:** funciones que solo calculan o buscan (`find`, `size`, `parse`) y las que devuelven un error o resultado que hay que comprobar (será clave en el decoder). **No** en funciones cuyo retorno es secundario a la acción que hacen.
* Ejemplo de la librería estándar: `std::vector::empty()` es `[[nodiscard]]` desde C++20 porque se escribía `v.empty();` creyendo que vaciaba el vector (eso es `clear()`).
* **Con mensaje (C++20):** `[[nodiscard("check for nullptr")]]`.
* **Ignorarlo a propósito:** `static_cast<void>(find(dict, "x"));`.
* **Error real encontrado:** escribí `[[no_discard]]`. Un atributo desconocido no es error: GCC lo **ignora y avisa** (`-Wattributes`); `-Werror` lo convirtió en error y evitó un `nodiscard` que no hacía nada.

### 8. Comentarios en el código
* **Pocos y explicando el porqué** (decisiones no obvias), no el qué. `// forward declaration` sobra; lo útil es *por qué* hace falta.
* **El comentario más valioso:** las decisiones de diseño que alguien podría "arreglar" sin saber el motivo (p. ej. por qué `Dict` no es un `std::map`).
* **En headers, documentar el contrato** de cada función: qué devuelve, cuándo devuelve `nullptr`, cuánto vive el resultado. Quien la llama no debería necesitar leer el `.cpp`.
* `///` o `/** ... */` son el formato de **Doxygen** (genera documentación web); `//` basta mientras no se use.

### 9. Fuentes: tipos incompletos en contenedores
* **Propuesta N4510**, *"Minimal incomplete type support for standard containers"* (Zhihao Yuan, 2015), aprobada para C++17: solo `std::vector`, `std::list` y `std::forward_list` (los que las implementaciones ya soportaban de forma fiable). Los asociativos (`std::map`...) quedaron fuera.
* **Estándar:** secciones `[vector.overview]`, `[list.overview]`, `[forward.list.overview]`. Borrador consultable en `eel.is/c++draft` (p. ej. `eel.is/c++draft/vector.overview`).
* **cppreference:** página de `std::vector`, parámetro `T`, nota *(since C++17)*. La de `std::map` no la tiene.
* **Por qué `std::map` funciona con GCC:** libstdc++ lo implementa como árbol de nodos enlazados por punteros. Es un **detalle de implementación**, no una garantía.
* Hábito: comprobar en la fuente las afirmaciones técnicas que justifican una decisión de diseño.

### 10. Estado y siguientes pasos
1. ✅ `Value` con las cuatro alternativas (`std::int64_t`, `std::string`, `List`, `Dict`).
2. ✅ `find()`: declarada con `[[nodiscard]]`, implementada en `bencode.cpp` y probada. *(Ver entrada siguiente.)*
3. ⬜ Empezar el decoder.

---

## [2026-11-09] Implementando `find()`: punteros, referencias, `auto` e inicialización

### 1. La implementación
```cpp
const Value* find(const Value& dictionary, std::string_view key) {
    const Dict* dict = std::get_if<Dict>(&dictionary.data);
    if (dict == nullptr) { return nullptr; }

    for (const auto& entry : *dict) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}
```
* **`std::get_if<Dict>(&dictionary.data)`:**
  * Necesita el tipo entre `< >`: no se puede deducir cuál de las alternativas se quiere.
  * Recibe la **dirección** del variant (`&`), no el variant: puntero de entrada, puntero de salida (`nullptr` si no es esa alternativa; incluso acepta un puntero nulo). Lo distingue de `std::get` (variant → excepción si falla).
  * El `const` del resultado se propaga solo: `dictionary` es `const` → `const Dict*`.
* **Guard clause / early return:** `if (dict == nullptr) { return nullptr; }` sale pronto y evita anidar el resto de la función. `if (!dict)` es equivalente (un puntero nulo se convierte a `false`): `== nullptr` es más explícito, `!dict` más compacto.
* **`[[nodiscard]]` solo en la declaración** (el header, que es lo que ve quien llama). Repetirlo en la definición no es error, pero puede desincronizarse.
* **Errores encontrados al compilar una versión a medias** (archivo sin guardar):
  * `unused parameter 'key'`.
  * `control reaches end of non-void function` (`-Wreturn-type`): una función que debe devolver algo y llega al final sin `return` es *undefined behavior* (devolvería basura). `-Wall` lo detecta, `-Werror` lo para.
* **Recordatorio:** guardar (`Ctrl+S`) antes de compilar; el compilador lee el disco, no el editor.

### 2. El `for` de rango y el *dangling pointer*
* **`*dict`:** `dict` es un puntero; el `for` necesita el contenedor, así que hay que **desreferenciarlo**.
* **Sin `&` (`const auto entry`):** en cada vuelta se crea una **copia** del par en la pila, que se destruye al acabar la vuelta. `return &entry.second;` devolvería la dirección de esa copia → *dangling pointer* (ASan: *stack-use-after-scope*). Además copia cada clave y cada `Value` (con `pieces`, 26800 bytes) para nada.
* **Con `&` (`const auto& entry`):** `entry` es **otro nombre para el elemento que ya está en el vector**. `&entry.second` apunta al `Value` real, que sigue vivo mientras exista el diccionario.
  ```
  Vector (heap):  [ ("length", 42) ][ ("name", "debian") ]
                          ▲
                          └── entry (referencia) → &entry.second apunta DENTRO del vector
  ```
* **Structured bindings (C++17):** desempaquetan el par en dos nombres, sin copia gracias a `const auto&`:
  ```cpp
  for (const auto& [k, v] : *dict) {
      if (k == key) { return &v; }
  }
  ```
  No se pueden llamar `key` (ya existe el parámetro): `-Wshadow` lo marcaría como error.
* **Experimento útil:** quitar el `&` a propósito y ver cómo ASan detecta el error.

### 3. Cómo funciona el `for` de rango por dentro
* Es azúcar sintáctico sobre `begin()`/`end()`:
  ```cpp
  for (const auto& entry : contenedor) { ... }
  // ≈
  auto it = contenedor.begin(); auto fin = contenedor.end();
  for (; it != fin; ++it) { const auto& entry = *it; ... }
  ```
* **Funciona con cualquier tipo**, no solo con la librería estándar:
  1. Arrays de C (el compilador conoce su tamaño).
  2. Clases con `begin()` y `end()` miembro que devuelvan un **iterador**: algo comparable con `!=`, avanzable con `++` y desreferenciable con `*` (un puntero cumple las tres).
  3. Tipos con funciones libres `begin(x)` / `end(x)`.
  4. Listas entre llaves: `for (int n : {1, 2, 3})`.
* Los contenedores estándar funcionan porque tienen `begin()`/`end()`, no por un trato especial. `Value` no los tiene: por eso primero se saca el `Dict`/`List` del variant.

### 4. Punteros y referencias
* **`.` vs `->`:**
  * `.` con un **objeto** o una **referencia** (una referencia *es* el objeto con otro nombre).
  * `->` con un **puntero** (una variable distinta que guarda una dirección). `p->first` ≡ `(*p).first`.

| Declaración | Qué es | Acceso a miembro |
|---|---|---|
| `Value v;` | objeto | `v.data` |
| `const Value& r = v;` | referencia | `r.data` |
| `const Value* p = &v;` | puntero | `p->data` o `(*p).data` |

* **`&` y `*` significan cosas distintas según dónde estén:**

| Símbolo | En una declaración (junto al tipo) | En una expresión (delante de una variable) |
|---|---|---|
| `&` | referencia: `Value& r` | dirección de: `&v` → puntero |
| `*` | puntero: `Value* p` | desreferenciar: `*p` → el objeto |

  En expresiones son opuestos: `&` va del objeto al puntero, `*` del puntero al objeto (`*&v` es `v`). `return &entry.second;` devuelve un **puntero** (operador dirección), no una referencia.

* **En C no existen las referencias:** solo punteros. "Pasar por referencia" en C significa pasar un puntero (`incrementa(&n)` con `void incrementa(int* x) { (*x)++; }`). C++ añadió `T&`; por debajo suele implementarse como un puntero, pero con garantías:

| | Puntero (`T*`) | Referencia (`T&`) |
|---|---|---|
| ¿Puede ser nulo? | sí (`nullptr`) | no |
| ¿Hay que inicializarlo? | no | sí, al declararla |
| ¿Puede cambiar a qué apunta? | sí | no, queda ligada para siempre |
| Aritmética (`p + 1`) | sí | no |
| Acceso a miembros | `->` | `.` |

* Se crearon para pasar objetos grandes sin copiar con sintaxis limpia y sin riesgo de nulo, y para la sobrecarga de operadores (`a + b`, `cout << x`).
* Pregunta a hacerse con cada `&` o `*` en una declaración: **¿es dueño, es una referencia o es un puntero que puede ser nulo?**

### 5. Referencias constantes (`const T&`)
* Referencia a través de la cual **no se puede modificar** el objeto (el objeto en sí no tiene por qué ser `const`).
* Forma estándar de pasar objetos grandes de solo lectura: **sin copia** (referencia) y con **promesa de no modificar** (`const`), visible en la firma y vigilada por el compilador.

| Firma | ¿Copia? | ¿Modifica el original? | Uso típico |
|---|---|---|---|
| `void f(Value v)` | sí | no (modifica su copia) | tipos pequeños (`int`, `string_view`) o cuando se quiere una copia |
| `void f(Value& v)` | no | **sí** | la función debe modificar el objeto de quien llama |
| `void f(const Value& v)` | no | no | objetos grandes de solo lectura |

* **Aceptan temporales** (las no constantes no): `f(const std::string&)` admite `f("hola")` porque C++ crea un `std::string` temporal que vive durante la llamada; modificar algo que va a desaparecer sería un error casi seguro.
* **Const-correctness:** el `const` se propaga en cadena. Como `dictionary` es `const Value&`, todo lo que sale de él es `const` (`const Dict*`, `const auto&`, `const Value*`): si `find` recibe el diccionario como solo lectura, no puede devolver una forma de modificarlo.

### 6. `auto`
* El compilador **deduce el tipo** a partir de la inicialización (C++11). Se decide **al compilar**: la variable tiene un tipo fijo (no es el `var` de JavaScript).
  ```cpp
  auto p = find(dict, "name");   // const Value*
  ```
* En el `for`, `const auto& entry` ahorra escribir `const std::pair<std::string, Value>& entry`.
* **Trampa:** `auto` solo deduce el tipo "limpio", **sin `&` ni `const`** → hace una copia:
  ```cpp
  const Value& ref = ...;
  auto a = ref;         // Value          → COPIA
  auto& b = ref;        // const Value&   → referencia
  const auto& c = ref;  // const Value&   → referencia de solo lectura (explícito)
  ```
  Tú decides copia o referencia; `auto` solo rellena el tipo base.
* **Cuándo:** tipo largo y obvio por el contexto (`for`, iteradores), tipo ya visible en la línea, tipos sin nombre (lambdas). **Mejor no** cuando oculta información importante (p. ej. si un entero es `int` o `int64_t`). *"Usa `auto` cuando el tipo no aporte nada al lector; escríbelo cuando sí."*

### 7. Construir pares e inicializar agregados
* **`< >` para tipos, `( )` / `{ }` para valores:** `std::pair<"length", 5>` no compila (son valores, no tipos).
  ```cpp
  dictionary.push_back(std::pair<std::string, btclient::Value>("length", btclient::Value{5})); // completo
  dictionary.push_back({"length", btclient::Value{5}});   // el vector ya sabe el tipo del par
  dictionary.emplace_back("length", btclient::Value{5});  // construye el par dentro del vector, sin temporal
  ```
* **`Value{5}` es necesario:** `Value` no tiene constructores propios, así que C++ no convierte un `5` en `Value` automáticamente.
  * Alternativa de diseño: darle constructores para cada alternativa y poder escribir `{"length", 5}`. Coste: escribirlos, y las conversiones implícitas tienen trampas (un literal `"hola"` podría elegir la alternativa equivocada). **Decisión:** de momento, construcción explícita; reconsiderar al escribir el decoder.
* **Prefijo `btclient::`:** necesario fuera del namespace (en `main.cpp`); dentro de `bencode.cpp` no. En un `.cpp` (nunca en un header): `using btclient::Value;` (mejor) o `using namespace btclient;`.
* **Inicialización de agregados** (heredada de C): las llaves asignan los valores a los miembros **en orden de declaración**; los que faltan se inicializan a cero.
  ```cpp
  struct Punto { int x; int y; };
  Punto p{1, 2};   // x = 1, y = 2
  Punto q{7};      // x = 7, y = 0
  ```
  `Value{5}` inicializa su único miembro, `data`.
  * **Inicializadores designados (C++20):** `btclient::Value v{.data = 5};` (los campos, en orden de declaración).
  * **Solo para agregados:** sin constructores propios, sin miembros privados/protegidos, sin funciones virtuales. Si `Value` recibe constructores, deja de ser agregado y `{...}` pasa a llamarlos.
* **Trampa del operador coma:** `push_back(("length", btclient::Value{5}))`. Los paréntesis interiores no crean un par: son una expresión, y la coma es el **operador coma** de C (`(a, b)` evalúa `a`, lo descarta y vale `b`). Llega solo un `Value` y el vector de pares no lo acepta; GCC avisa también de que el operando izquierdo no tiene efecto.
  * `( )` sueltos → agrupan una expresión.
  * `{ }` → construyen un objeto del tipo esperado.
  * `Tipo( )` → construye (los paréntesis solo construyen con un tipo delante).

### 8. Comentarios: estándares
* **No hay un estándar oficial** en C++; cada proyecto sigue una guía. Referencias: **Google C++ Style Guide** (la más citada, con sección de comentarios), **LLVM Coding Standards**, **C++ Core Guidelines** (sección NL).

| Dónde | Qué comentar |
|---|---|
| Header | **qué hace** y su **contrato** (parámetros, retorno, errores, vida útil del resultado): documentación para quien la *usa* |
| `.cpp` | **por qué** se hace algo de forma no obvia: documentación para quien la *mantiene* |
| En ningún sitio | lo que el código ya dice |

* `find` no necesita comentarios en el `.cpp`: el contrato está en el header y la implementación se explica sola.
* `TODO` para pendientes (`// TODO(chefo): validate that keys are sorted`), fáciles de buscar con grep.
* Cabecera de archivo (licencia, descripción): opcional en un proyecto personal. Doxygen: solo si se genera documentación.
* **Decisión:** usar la Google C++ Style Guide como referencia en nombres y comentarios, y ser coherente. Pendiente: elegir si se indenta dentro de los `namespace` (Google no lo hace) y aplicarlo en todos los archivos.

### 9. Lecciones de la prueba de `find`
* Resultado: los tres casos correctos (clave que existe, clave que no, `Value` que no es diccionario).
* **Comprobar el valor, no solo que exista:** si `find` devolviera el `Value` equivocado, "no es `nullptr`" seguiría pasando. Se comprueba con `->` (`resultado->data`).
* **Las pruebas que solo imprimen al acertar fallan en silencio:** si algo va mal, simplemente falta una línea. Una prueba debe avisar claramente al fallar → argumento para un **framework de tests** (GoogleTest).
* Nombres claros en las pruebas (no invertir `dict`/`dictionary` respecto a `find`).
* Las pruebas temporales en `main.cpp` no se suben: `git restore src/main.cpp`.

### 10. Estado y siguientes pasos
1. ✅ `Value` con cuatro alternativas.
2. ✅ `find()` implementada y probada a mano.
3. ⬜ Montar **GoogleTest** con CMake (`FetchContent`) y pasar la prueba de `find` a un test de verdad.
4. ⬜ Diseñar el decoder: firma (¿`std::string_view` de entrada?), estrategia de errores, cómo llevar la posición de lectura.

---

## [2026-11-09] Igualdad de `Value`, `const` en punteros y cómo leer errores

### 1. `operator==` generado por el compilador (C++20)
* Un `struct` **no tiene `==` por defecto**. Desde C++20 se puede pedir que el compilador lo genere comparando miembro a miembro:
  ```cpp
  struct Value {
      std::variant<std::int64_t, std::string, List, Dict> data;
      bool operator==(const Value&) const = default;
  };
  ```
  * `operator==` es la función que se llama al escribir `a == b`.
  * `= default`: "escríbela tú". Compara `data` con `data`; el `variant` se compara por alternativa activa y valor, y es **recursivo** (dos listas son iguales si lo son sus elementos). Funciona con el tipo recursivo (comprobado con GCC 15 y las flags del proyecto).
* **Por qué:** los tests del decoder compararán árboles enteros ("decodificar `li1ei2ee` debe dar la lista `[1, 2]`") y GoogleTest usa `==` (`EXPECT_EQ`).
* **Tiene que ir dentro del `struct`.** Fuera, GCC lo toma como función libre y da dos errores:
  * *"non-member function cannot have cv-qualifier"*: el `const` final ("no modifica el objeto sobre el que se llama") solo existe en funciones **miembro**.
  * *"must have exactly two arguments"*: un `==` libre necesita los dos lados como parámetros; como miembro, el lado izquierdo es el propio objeto.
* **Uso:** comparar `Value` con `Value`, desreferenciando el puntero:
  ```cpp
  const btclient::Value* item = btclient::find(dict, "length");
  if (item != nullptr && *item == btclient::Value{5}) { ... }
  ```
  * `item` → puntero; `*item` → el `Value` (el `*` va **delante**: `item*` se lee como una multiplicación incompleta → *"expected primary-expression before '=='"*); `item->data` → el `variant` de dentro.
  * Regla mental: **un `Value` es una caja, `.data` es lo que hay dentro; para comparar, cajas con cajas.**

### 2. `std::variant` solo se compara con otro `variant`
* `item->data == btclient::Value{5}` → error: `variant` vs `Value`.
* `item->data == std::int64_t{5}` → error: `variant` no define `==` contra una de sus alternativas, solo contra otro `variant` del mismo tipo.
* Formas válidas:
  1. `*item == btclient::Value{5}` (con el `operator==` de `Value`).
  2. `get_if` + comparar el valor: `const std::int64_t* n = std::get_if<std::int64_t>(&item->data); if (n && *n == 5)`.
  3. `holds_alternative` + `get`.
* Para **comparar**, cajas con cajas; `get`/`get_if`/`holds_alternative` son para **sacar** el contenido y usarlo.
* Escribir siempre `std::int64_t`: `<cstdint>` suele exponer también `int64_t` en el ámbito global, pero solo `std::` está garantizado.

### 3. `const` en punteros
* `find` devuelve `const Value*` ("puedes mirar, no modificar"). Guardarlo en un `Value*` da *"invalid conversion from 'const btclient::Value\*' to 'btclient::Value\*'"*: C++ **no permite quitar un `const` en silencio** (rompería la promesa de `find`); añadirlo sí (solo restringe más).
  ```cpp
  const btclient::Value* item = btclient::find(dict, "length");
  const auto* item = btclient::find(dict, "length");   // auto conserva el const; const auto* lo hace visible
  ```
* **Declarar e inicializar en la misma línea.** `Value* item;` seguido de `item = ...;` deja un puntero **sin inicializar** (apunta a una dirección cualquiera) entre ambas líneas. Regla: declarar cada variable donde se necesita y darle valor ahí mismo.

### 4. Evaluación en cortocircuito
* En `a && b`, si `a` es `false`, **`b` no se evalúa** (con `||`, si `a` es `true`).
* Por eso es seguro `item != nullptr && *item == ...`: si `item` es nulo, nunca se desreferencia. **El orden importa:** la comprobación de nulo va primero.

### 5. Cómo leer errores largos de plantillas
* Un error con tipos de la librería estándar puede ocupar cientos de líneas. **Lo importante está en la primera línea con `error:`**:
  ```
  main.cpp:12:39: error: no match for 'operator==' (operand types are
    'const std::variant<long int, std::__cxx11::basic_string<...>, ...>' and 'btclient::Value')
  ```
  → archivo, línea, columna, y qué tipos intervienen.
* Lo que sigue (*"there are 25 candidates"*, `allocator`, `error_code`, `monostate`...) es GCC listando todos los `==` que conoce y por qué no encajan. Casi nunca hace falta; como mucho, el candidato que es tuyo (aquí: *"no known conversion for argument 1 from 'variant' to 'const Value&'"*).
* `std::int64_t` aparece como `long int` y `std::string` como `std::__cxx11::basic_string<char, ...>`: son sus nombres reales en libstdc++.

### 6. IntelliSense vs el estándar de C++
* VS Code marcaba `= default` en `operator==` con *"'= default' solo puede aparecer en los constructores predeterminados..."* (C/C++ 1774), aunque compilaba.
* **Causa:** IntelliSense no lee el `CMakeLists.txt` y asumía un estándar anterior a C++20 (antes de C++20, `= default` solo valía para constructores, destructores y asignaciones).
* **Solución:** usar CMake Tools como proveedor de configuración, en `.vscode/settings.json`:
  ```json
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"
  ```
  IntelliSense usa así las mismas flags que CMake (`-std=c++20`, rutas de include). Alternativa parcial: `"C_Cpp.default.cppStandard": "c++20"` (hay que mantenerla a mano). Tras cambiarlo: `Ctrl+Shift+P` → *Developer: Reload Window*.
* Mismo problema de fondo que con las rutas de `#include`: **editor y compilador deben ver la misma configuración**. Ante la duda, manda el compilador.

---

## Git: formato de commits y flujo de trabajo

### 1. Conventional Commits
Formato usado en el proyecto ([conventionalcommits.org](https://www.conventionalcommits.org)):
```
<tipo>(<ámbito opcional>): <descripción>

<cuerpo opcional>

<pie opcional>
```
* Ejemplos reales del repo:
  * `build: add btcore library and shared build options target`
  * `feat(bencode): implement find() for dictionary key lookup`
  * `docs: add find() implementation notes to learning log`
  * `chore: open .torrent files as read-only in VS Code`
  * `style: fix namespace closing comment in bencode.cpp`

### 2. Tipos
| Tipo | Cuándo | Ejemplo |
|---|---|---|
| `feat` | funcionalidad nueva | `feat(bencode): add list alternative to Value` |
| `fix` | arreglo de un bug | `fix(bencode): reject negative string lengths` |
| `docs` | solo documentación (README, bitácora, roadmap) | `docs: add project roadmap` |
| `test` | añadir o cambiar tests | `test(bencode): add find() unit tests` |
| `refactor` | cambiar código sin cambiar comportamiento | `refactor: add btclient namespace and fix include paths` |
| `style` | formato, espacios, comentarios mal colocados (sin cambio de lógica) | `style: fix namespace closing comment` |
| `build` | sistema de compilación o dependencias (CMake) | `build: fetch GoogleTest with FetchContent` |
| `ci` | integración continua (GitHub Actions) | `ci: build and test on every push` |
| `chore` | mantenimiento y configuración que no encaja en lo anterior (`.vscode`, `.gitignore`) | `chore: insert final newline on save in VS Code` |
| `perf` | mejora de rendimiento | `perf: avoid copying piece hashes` |

### 3. Ámbito (opcional)
* Entre paréntesis tras el tipo: la parte del proyecto afectada → `feat(bencode)`, `fix(tracker)`, `test(peer)`.
* Útil cuando el proyecto tiene varios módulos; se omite si el cambio es general.

### 4. Reglas de la descripción (título)
* **Imperativo**, como una orden: "add", "fix", "remove" (no "added", "adding", "adds"). Truco: debe completar la frase *"If applied, this commit will..."*.
* **En minúscula tras los dos puntos y sin punto final.**
* **Corto:** ≤ ~72 caracteres (GitHub corta los más largos).
* **Qué cambia, no cómo:** "add find() for dictionary key lookup", no "add a for loop over the vector".
* **En inglés** (portfolio, estándar de la industria).

### 5. Cuerpo y pie (opcionales)
* **Cuerpo:** separado del título por una línea en blanco. Explica el **porqué** o detalla un cambio con varias partes (viñetas con `-`).
  ```bash
  git commit -m "chore: open .torrent files as read-only in VS Code" -m "Prevents the editor from corrupting the binary piece hashes when saving."
  ```
  Cada `-m` es un párrafo: el primero es el título, el segundo el cuerpo.
* **Pie:** referencias o avisos. `BREAKING CHANGE: ...` (o `feat!:`) para cambios incompatibles; `Closes #12` para cerrar un issue.

### 6. Un commit = un cambio lógico
* Si para describirlo hace falta "y" ("add X **and** fix Y"), probablemente son dos commits.
* Mejor varios commits pequeños que uno enorme: el historial se lee como una historia y es fácil encontrar (o revertir) un cambio concreto.
* Separar código y documentación (`feat` por un lado, `docs` por otro).
* No subir pruebas temporales ni código comentado (`git restore src/main.cpp`).

### 7. Flujo de `git add`
1. `git status` → qué ha cambiado.
2. Agrupar por **motivo**, no por archivo.
3. `git add <archivos>` → solo los de ese cambio. **Evitar `git add .`** (mete todo; así se coló el `.torrent` en el commit del roadmap).
   * `git add -p` → elegir trozo a trozo si un archivo tiene cambios de motivos distintos (interactivo).
4. `git diff --staged` → revisar exactamente qué entra en el commit.
5. `git commit -m "tipo(ámbito): descripción"`.
6. Repetir hasta que `git status` diga `working tree clean`.
7. `git push` (la primera vez en una rama nueva: `git push -u origin <rama>`).

### 8. Deshacer cosas (antes de hacer push)
| Situación | Comando |
|---|---|
| Sacar un archivo del stage (sin perder cambios) | `git restore --staged <archivo>` |
| Descartar los cambios de un archivo (¡se pierden!) | `git restore <archivo>` |
| Deshacer el último commit conservando los cambios en el stage | `git reset --soft HEAD~1` |
| Corregir el último commit (mensaje o autor) | `git commit --amend` (`--reset-author --no-edit` para el autor) |
* **Solo antes de hacer push:** reescribir commits ya subidos obliga a `push --force` y descoordina a cualquiera que los tenga.

### 9. Ramas
* Trabajo en ramas de funcionalidad (`parser`), no directamente en `main`.
* Cuando la funcionalidad esté completa: **Pull Request** de la rama a `main` en GitHub y fusionarla, aunque se trabaje solo (flujo profesional y queda visible en el perfil).
* Versiones: `git tag v0.2.0` + `git push --tags` al cumplir un hito.

---

## [2026-11-09] Tests unitarios con GoogleTest

### 1. Conceptos
* **Test:** código que comprueba que otro código funciona (la prueba de `find` en `main.cpp` ya lo era).
* **Test unitario:** prueba **una pieza pequeña** (una función) de forma aislada, un caso por test.
* **Problemas de las pruebas a mano en `main.cpp`:** se borraban tras usarlas (`git restore`) y fallaban **en silencio** (solo imprimían al acertar).
* **GoogleTest:** la librería de tests más usada en C++ (Google). Los tests quedan guardados en `tests/`, en un ejecutable propio separado del cliente, y cada fallo se informa con archivo, línea, valor esperado y valor obtenido.
* **Por qué importa:** el decoder tendrá decenas de casos (enteros, negativos, vacíos, anidados, datos corruptos); los tests detectan al momento si un cambio rompe algo que funcionaba (regresiones). En un portfolio C++ destacan.

### 2. Integración con CMake
```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(btclient_tests tests/bencode_test.cpp)
target_link_libraries(btclient_tests PRIVATE btcore btclient_options GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(btclient_tests)
```
* **`FetchContent`:** descarga código de terceros al configurar y lo integra como targets de CMake, sin instalar nada en el sistema. Queda en `build/_deps/` (`googletest-src`, `-build`, `-subbuild`). La primera configuración necesita internet.
* **Versión fija:** URL de una release concreta (`v1.18.0`) o de un commit concreto (hash en la URL), nunca "la última": el build no cambia solo. La URL de release es más legible.
* **`URL_HASH SHA256=...`:** CMake verifica el archivo descargado; si no coincide (manipulado o corrupto), se niega a usarlo. Buena práctica de seguridad con dependencias.
  * Calcularlo: `curl -sL <url> | sha256sum`.
  * La salida `6e31...d8b5  -` → el hash son solo los **64 caracteres hex** (256 bits = 32 bytes, 2 hex por byte). Los dos espacios son separador y `-` significa "entrada estándar" (los datos llegaron por la tubería, no de un archivo).
  * Error encontrado: `SHA256=SHA256=...` (duplicado) → *"URL_HASH is set to..."*, configuración rechazada. Formato correcto: `ALGORITMO=hash`, una vez.
  * Relación con el proyecto: los hashes de las piezas son **SHA-1** (20 bytes = 40 hex), pero en el `.torrent` van en **binario**, no en hex.
* **`DOWNLOAD_EXTRACT_TIMESTAMP TRUE`:** evita el aviso de la política CMP0135 (fecha de los archivos descomprimidos).
* **`gtest_force_shared_crt`** (aparece en la guía oficial): solo afecta a Windows/MSVC (runtime de C). En Linux no hace nada.
* **`enable_testing()`:** activa CTest.
* **`GTest::gtest_main`:** GoogleTest + un `main()` ya hecho → el archivo de tests no lleva `main`.
* **`gtest_discover_tests`:** CTest registra cada `TEST` por separado.
* **Los warnings estrictos no afectan a GoogleTest:** van en `btclient_options`, que GoogleTest no enlaza. Con flags globales, `-Werror` podría romper el build por código ajeno.

### 3. Mejoras opcionales (pendientes)
* **gMock:** segunda librería incluida en GoogleTest para **objetos simulados** (*mocks*), p. ej. un tracker falso que responde lo que se le diga, sin red real; también verifica que el código llamó a algo con ciertos argumentos. Útil en la fase de red; ahora no. Se puede desactivar con `set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)` antes de `FetchContent_MakeAvailable` para compilar menos.
* **Tests desactivables (`BUILD_TESTING`):** `include(CTest)` crea esta opción (y llama a `enable_testing()`). Envolviendo la sección de tests en `if(BUILD_TESTING) ... endif()`, quien solo quiera el cliente usa `-DBUILD_TESTING=OFF` y no descarga GoogleTest. Por defecto `ON`. Convención que esperan gestores de paquetes y otros proyectos.

### 4. Cómo funciona GoogleTest
```cpp
TEST(Find, ReturnsValueForExistingKey) {
    btclient::Value dictionary{btclient::Dict{{"length", btclient::Value{5}}}};

    const btclient::Value* item = btclient::find(dictionary, "length");

    ASSERT_NE(item, nullptr);
    EXPECT_EQ(*item, btclient::Value{5});
}
```
* **`TEST(Grupo, Caso)`:** macro que crea una función y la **registra** en una lista interna. Nombre completo: `Find.ReturnsValueForExistingKey`. El `main` de `gtest_main` recorre la lista y ejecuta todos.
* **Patrón Arrange-Act-Assert:** preparar / ejecutar / comprobar, separados por líneas en blanco.
* **Nombres:** grupo = lo que se prueba; caso en tercera persona describiendo el comportamiento (`ReturnsNullForMissingKey`).
* **`EXPECT_` vs `ASSERT_`:**
  * `EXPECT_*`: si falla, lo apunta y **sigue** (se ven todos los fallos). **Por defecto.**
  * `ASSERT_*`: si falla, **termina ese test**. Solo cuando seguir no tiene sentido o es peligroso (p. ej. `ASSERT_NE(item, nullptr)` antes de `*item`). Leer un `ASSERT_` comunica "lo de abajo depende de esto".
  * En la última línea del test son equivalentes, pero por convención `EXPECT_`.
  * Un test que falla no detiene a los demás.
* **Comprobar el valor exacto esperado:** `EXPECT_EQ(*item, Value{...})` es mejor que `EXPECT_NE(*item, Value{5})`, que solo descarta un valor y deja pasar cualquier otro error. `EXPECT_NE` solo cuando lo que importa es la diferencia.
* **No desreferenciar sin comprobar:** `EXPECT_EQ(find(d, "k")->data, ...)` revienta (SEGV) si `find` devuelve `nullptr`. Guardar en una variable, `ASSERT_NE`, y luego usar.
* `EXPECT_EQ` usa el `operator==` de `Value` (motivo para haberlo creado). Comparar direcciones también vale: `EXPECT_EQ(item, &entries[0].second)` comprueba que `find` no devuelve una copia.

### 5. Tests escritos (grupo `Find`, 7 tests)
| Test | Qué protege |
|---|---|
| `ReturnsValueForExistingKey` | el caso normal |
| `ReturnsNullForMissingKey` | "no encontrado" es habitual |
| `ReturnsNullForEmptyDict` | caso límite: el bucle no da ninguna vuelta |
| `ReturnsNullForNonDict` | string, entero y lista → `nullptr` (el `get_if`) |
| `RequiresExactKeyMatch` | prefijo (`"len"`), más larga (`"lengthy"`), mayúsculas (`"Length"`) y vacía (`""`) no coinciden: comparación exacta, no "empieza por"/"contiene" (las claves bencode son bytes, `L` ≠ `l`) |
| `FindsCorrectKeyAmongSeveral` | no devuelve siempre el primer elemento |
| `ReturnsPointerIntoDictionary` | el puntero apunta dentro del diccionario, no a una copia (protege el `const auto&`) |
* **Prueba de fuego:** romper un test a propósito (esperar 6 en vez de 5) para comprobar que los tests detectan fallos de verdad. Funcionó: 6 pasan, 1 falla, con resumen final.

### 6. Compilar y ejecutar
| Quiero... | Comando |
|---|---|
| configurar (primera vez, tras borrar `build/` o cambiar una opción) | `cmake -B build` (`-B` mayúscula) |
| compilar todo | `cmake --build build` |
| compilar solo los tests | `cmake --build build --target btclient_tests` |
| compilar solo el cliente | `cmake --build build --target bittorrent_client` |
| ejecutar los tests (detallado, en color) | `./build/btclient_tests` |
| ejecutar todos vía CMake | `ctest --test-dir build --output-on-failure` |
* `--target X` compila X **y lo que necesite** (si cambia `bencode.cpp`, recompila `btcore` para los tests, no el cliente). La compilación es incremental.
* **Ni `ctest` ni `./build/btclient_tests` compilan:** ejecutan el binario que hay en disco. Orden siempre: **compilar → ejecutar**.
* Mientras se programa: ejecutable directo con filtros. Antes de un commit (y en CI): `ctest`.

### 7. Elegir qué tests ejecutar
| Comando | Qué hace |
|---|---|
| `./build/btclient_tests --gtest_list_tests` | lista sin ejecutar |
| `--gtest_filter='Find.*'` | solo el grupo `Find` |
| `--gtest_filter=Find.ReturnsNullForMissingKey` | un test concreto |
| `--gtest_filter='*Null*'` | los que contengan "Null" |
| `--gtest_filter='Find.*-*Missing*'` | los de `Find` menos los que contengan "Missing" |
| `--gtest_brief=1` | solo muestra los que fallan |
| `--gtest_repeat=100` | repite (fallos intermitentes) |
| `--gtest_shuffle` | orden aleatorio (detecta tests dependientes entre sí) |
| `--help` | todas las opciones |
* Patrones: `*` = cualquier cosa, `:` separa patrones, `-` excluye. Entre comillas para que la terminal no interprete el `*`.
* CTest: `ctest --test-dir build -R Find` (expresión regular), `-N` lista sin ejecutar.
* VS Code: panel **Testing** (icono del matraz) con CMake Tools: ejecutar con un clic, ✗ roja en los que fallan.

### 8. Leer un fallo de GoogleTest
```
bencode_test.cpp:11: Failure
Expected equality of these values:
  *item
    Which is: 40-byte object <06-00 00-00 00-00 00-00 BE-BE ... 00-BE BE-BE ...>
  btclient::Value{5}
    Which is: 40-byte object <05-00 00-00 ... 00-00>
```
* Archivo y línea, el texto literal de cada lado y su valor.
* **Volcado de bytes:** GoogleTest no sabe imprimir `Value`, así que muestra la memoria en hexadecimal:
  * `40-byte object`: `sizeof(Value)` = 32 bytes de almacenamiento (la alternativa más grande, `std::string` en libstdc++) + 1 byte de **índice del variant** + relleno de alineación.
  * `06-00 00-00 00-00 00-00`: el `std::int64_t` 6 en **little-endian** (byte menos significativo primero).
  * El `00` antes de los últimos `BE`: índice del variant = 0 → alternativa `std::int64_t`.
  * `BE-BE...`: bytes nunca escritos. **ASan rellena la memoria recién reservada en el heap con `0xBE`** para delatar lecturas sin inicializar. `*item` vive en el heap (dentro del vector); el temporal `Value{5}` en la pila, a ceros.
* **Pendiente:** enseñar a GoogleTest a imprimir `Value` con `PrintTo(const Value&, std::ostream*)`, reutilizando el futuro printer → los fallos mostrarán `6` vs `5`.

### 9. Errores dentro de macros: `required from here`
* `EXPECT_EQ(item, btclient::Value{5})` (puntero vs `Value`) da el error **dentro de `gtest.h`**: la comparación ocurre en el código de la macro.
  ```
  .../gtest/gtest.h:1399:11: error: no match for 'operator==' (operand types are 'const btclient::Value* const' and 'const btclient::Value')
  .../tests/bencode_test.cpp:11:5:   required from here
  ```
* La línea `error:` dice **qué** (los tipos: con `*` es puntero); la línea **`required from here`** dice **dónde** en el código propio.

### 10. `SCOPED_TRACE` y RAII
```cpp
{
    SCOPED_TRACE("integer");
    btclient::Value value{5};
    EXPECT_EQ(btclient::find(value, "length"), nullptr);
}
```
* Añade un texto a los mensajes de fallo producidos **mientras está activa**: desde su línea hasta el final del bloque `{ }`. En el fallo aparece `Google Test trace: bencode_test.cpp:50: integer`.
* Un bloque `{ }` por caso: la traza solo vale dentro, y cada bloque puede reutilizar el nombre `value`.
* **Por dentro:** crea un objeto local; su constructor apunta el mensaje en una lista interna y su destructor (al cerrar `}`) lo quita. Aprovechar la destrucción automática de un objeto al salir de su bloque se llama **RAII** (*Resource Acquisition Is Initialization*): así funcionan `std::vector`, `std::unique_ptr`, `std::lock_guard`, y así se gestionarán sockets y archivos.
* Útil con varios casos parecidos en un test, en bucles sobre casos (decoder) y en funciones auxiliares de test.

### 11. Construir `Value`s: conversiones implícitas
* **Regla práctica: al meter algo en un `Value`, envolverlo siempre en `btclient::Value{...}`.**
  ```cpp
  btclient::Value dictionary{btclient::Dict{
      {"length",  btclient::Value{5}},
      {"comment", btclient::Value{std::string("bittorrent_client")}},
  }};
  ```
  Un par por línea; la coma tras el último elemento está permitida (diffs más limpios al añadir otro).
* `{"comment", std::string("...")}` en un `Dict` **no compila**: *"converting to 'std::pair<...>' from initializer list would use explicit constructor"* → haría falta convertir `std::string` en `Value`.
* **Por qué `find(d, "length")` convierte solo y `Value` no:**
  1. Una conversión implícita necesita un **constructor no `explicit` en el tipo de destino**. `std::string_view` tiene uno desde `const char*`; `Value` (agregado sin constructores) no tiene ninguno.
  2. Como mucho **una conversión "de usuario" seguida**: `"texto"` → `std::string` → `variant` → `Value` serían varios saltos; C++ no los encadena en silencio.
* `Value{"dummy"}` sí compila: las llaves rellenan `data`, y queda un solo salto (`"dummy"` → `variant`, que elige `std::string`). Escribir `std::string(...)` ahí es para no depender de que el variant adivine (en versiones antiguas un literal podía acabar en `bool`).

| Expresión | ¿Compila? | Por qué |
|---|---|---|
| `find(d, "length")` (parámetro `string_view`) | ✅ | constructor de `string_view` desde `const char*` (1 salto) |
| `Value{"dummy"}` | ✅ | las llaves rellenan `data`; `"dummy"` → `variant` (1 salto) |
| `Value{std::string("dummy")}` | ✅ | igual, sin que el variant adivine |
| `{"comment", std::string("...")}` en un `Dict` | ❌ | haría falta `std::string` → `Value`, y `Value` no tiene constructor |
| `{"comment", btclient::Value{std::string("...")}}` | ✅ | el `Value` se construye explícitamente |
* Con constructores en `Value`, la cuarta fila funcionaría: decisión pendiente para el decoder. Si aparece *"would use explicit constructor"* o *"no matching conversion"* → falta un `btclient::Value{...}`.

### 12. Trampa de los vectores: `{}` vs `()`
```cpp
std::vector<int> a{5};   // [5]              (llaves → lista de elementos)
std::vector<int> b(5);   // [0, 0, 0, 0, 0]  (paréntesis → tamaño)
std::vector<int> c{};    // []
```
* `btclient::Dict{}` → constructor por defecto: vector vacío (`size() == 0`; normalmente `capacity() == 0`, sin reservar memoria hasta el primer elemento).

### 13. VS Code: kit de CMake Tools e IntelliSense
* **Kit:** qué compilador usa CMake Tools. Hasta elegirlo no configura nada e IntelliSense no recibe la configuración (por eso `<gtest/gtest.h>` salía en rojo).
  * Comprobar antes que la ventana está en WSL (esquina inferior izquierda: `WSL: Ubuntu`).
  * `Ctrl+Shift+P` → *CMake: Select a Kit* → GCC 15.2 (`[Scan for kits]` si no aparece; nunca `[Unspecified]`). Después *CMake: Configure* (salida en *Output → CMake/Build*).
  * En versiones recientes el kit **no está en la barra inferior**: está en el **panel de CMake** (icono del triángulo con llave en la barra izquierda) → *Project Status* → *Kit*.
  * Diagnóstico: *C/C++: Log Diagnostics* → debe mostrar el proveedor `ms-vscode.cmake-tools`, `c++20` y los includes de GoogleTest.
  * Plan B: `CMAKE_EXPORT_COMPILE_COMMANDS ON` + `"C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json"`.
* **Falso positivo restante:** `EXPECT_EQ(item, &entries[0].second)` subrayado con *"ninguna instancia de función sobrecargada EqHelper::Compare coincide... (..., `<tipo de error>`)"*. El motor de IntelliSense (EDG) no consigue resolver el tipo a través del tipo recursivo (`pair` con `Value` incompleto) y se rinde. GCC lo compila con `-Werror` y el test pasa → **manda el compilador**. Si se vuelve frecuente: clangd (analiza con Clang).

### 14. Estado y siguientes pasos
1. ✅ Infraestructura de tests (GoogleTest + CTest) y 7 tests de `find`.
2. ✅ Tests de `ValueEquality`. *(Ver entrada siguiente.)*
3. ⬜ (Opcional) `BUILD_TESTING`, `BUILD_GMOCK OFF`.
4. 🚧 Diseñar el decoder.

---

## [2026-11-10] Tests de igualdad, diseño del decoder y bytes con signo

### 1. Tests de `ValueEquality` (11 tests)
* **Qué se prueba:** el `operator==` de `Value`. `EXPECT_EQ(a, b)` evalúa por dentro `a == b`; `EXPECT_NE(a, b)` evalúa `a != b`, que en C++20 el compilador genera automáticamente a partir de `==`.
  * `EXPECT_TRUE(a == b)` haría lo mismo, pero al fallar solo dice "expected true"; `EXPECT_EQ` muestra los dos valores.
* **¿Por qué probar un `= default`?** Su comportamiento depende de cosas invisibles en esa línea: cómo compara `std::variant` (alternativa y valor), que sea recursivo a través de `List`/`Dict`, que el orden de `Dict` importe. Además, los tests del decoder usarán `EXPECT_EQ` entre `Value`s: si `==` fallara, darían por buenos resultados erróneos. Y protegen frente a cambios futuros (un `==` escrito a mano, `Dict` → `std::map`).
* **Cobertura:** las cuatro alternativas (enteros, strings, listas, diccionarios), alternativas distintas con "mismo valor" (`5` vs `"5"`; `List{}` vs `Dict{}`), recursividad (listas con elementos) y orden de claves.

| Test | Qué comprueba |
|---|---|
| `EqualIntegersAreEqual` / `DifferentIntegersAreNotEqual` | `5 == 5`, `5 != 6` |
| `EqualStringsAreEqual` / `DifferentStringsAreNotEqual` | `"string" == "string"`, `"string1" != "string2"` |
| `IntegerIsNotEqualToString` | `5 != "5"`: el `==` compara también la **alternativa** |
| `EqualListsAreEqual` / `DifferentListsAreNotEqual` | `[1, "a"]` igual a sí misma; distinta de `[1, "b"]` y de `[1]` |
| `EmptyListIsNotEqualToEmptyDict` | dos "vacíos" de tipos distintos |
| `EqualDictsAreEqual` / `DifferentDictsAreNotEqual` | distinta clave, distinto valor, distinta longitud |
| `DictKeyOrderMatters` | `{"a":1,"b":2} != {"b":2,"a":1}` |

* **Regla: en un test de "son distintos", cambiar una sola cosa.** `{"a":1}` vs `{"b":2}` cambia clave y valor a la vez: pasaría aunque `==` solo mirase las claves. Separado en "misma longitud, distinta clave" (`{"a":1}` vs `{"b":1}`) y "misma longitud, distinto valor" (`{"a":1}` vs `{"a":2}`), cada caso demuestra exactamente qué comprueba.
* **`EXPECT_NE` aquí sí es lo correcto:** lo que se quiere comprobar es precisamente la diferencia (excepción a "preferir `EXPECT_EQ` con el valor exacto").
* **Los tests también documentan decisiones de diseño:** `DictKeyOrderMatters` deja por escrito (con comentario) que el orden importa porque `Dict` es un vector, y que es correcto porque el bencode válido exige claves ordenadas. Si alguien cambia `Dict` por `std::map`, el test falla y le obliga a pensar si es intencionado.
* **Estilo:** `a`/`b` (o `lhs`/`rhs`) como nombres en tests de igualdad; etiquetas de `SCOPED_TRACE` coherentes; `std::string("...")` explícito aunque un literal también funcionaría (solo `std::string` puede construirse desde `const char*` entre las cuatro alternativas), por claridad y para no depender de la deducción del variant.
* **Comprobaciones antes de un commit:** `ctest --test-dir build` y `git diff --check` (detecta espacios sobrantes y problemas de formato).

### 2. Qué tiene que hacer el decoder
* Lo contrario de crear un `.torrent`: **bytes bencode → árbol de `Value`s**.
  ```
  "d6:lengthi5e4:name4:spame"  →  Value{ Dict{ {"length", 5}, {"name", "spam"} } }
  ```
* **Reglas (BEP 3):** el primer byte indica el tipo.

| Primer byte | Tipo | Formato | Ejemplo | Resultado |
|---|---|---|---|---|
| `i` | entero | `i` + número + `e` | `i42e` | `42` |
| `0`–`9` | string | longitud + `:` + bytes | `4:spam` | `"spam"` |
| `l` | lista | `l` + valores + `e` | `l4:spami42ee` | `["spam", 42]` |
| `d` | diccionario | `d` + (clave, valor)... + `e` | `d3:cow3:mooe` | `{"cow": "moo"}` |

* **Funcionamiento:** lleva una **posición** en el buffer; mira el byte actual, decide qué tipo empieza ahí, lo lee entero y avanza. Para cada elemento de una lista o diccionario **se llama a sí mismo** (recursivo, como `Value`).
  ```
  l4:spami42ee
  ^              'l' → lista; avanzo y leo valores hasta 'e'
   ^             '4' → string de longitud 4; salto ':'
     ^^^^        4 bytes: "spam"
         ^       'i' → entero hasta 'e': 42
             ^   'e' → fin de la lista → ["spam", 42]
  ```
* **Lo difícil: rechazar entrada inválida sin romperse nunca** (los datos vienen de fuera y pueden estar corruptos o manipulados):
  * **Truncados:** `i42` (falta `e`), `10:spam` (longitud que miente → *buffer overflow* si no se valida), `l4:spam` (lista sin cerrar).
  * **Formato inválido:** `i4x2e`, `i-0e` (prohibido), `i03e` (ceros a la izquierda prohibidos; solo `i0e`), `ie` (vacío), `x` (no inicia ningún tipo).
  * **Diccionarios:** claves que no son strings (`di1ei2ee`), claves desordenadas o repetidas.
  * **Números que no caben:** `i99999999999999999999e` (no cabe en `std::int64_t`), longitudes absurdas.
  * **Basura al final:** `i42eXYZ`. Un `.torrent` debe ser exactamente **un** valor.
  * **Anidamiento malicioso:** `llll...` un millón de veces → una llamada recursiva por nivel → *stack overflow*. Hace falta un **límite de profundidad**.
* **Info hash (a futuro):** el decoder tendrá que poder indicar dónde empezaba y terminaba el diccionario `info` en el buffer (SHA-1 sobre los bytes originales).

### 3. Plan incremental (divide y vencerás)
1. Enteros correctos (`i42e`, `i0e`, `i-7e`).
2. Enteros mal formados, un test por error.
3. Strings y sus errores.
4. Listas (aparece la recursividad, reutilizando enteros y strings).
5. Diccionarios y validación de claves ordenadas/únicas.
6. Extras: basura al final, límite de profundidad, y decodificar el `.torrent` de Debian.
* Cada paso es pequeño y se prueba antes del siguiente. Las partes difíciles de C++ ya están superadas; el decoder es sobre todo **lógica**.

### 4. Cómo se trabaja en proyectos grandes
* **Desarrollo incremental:** versión mínima que funciona, ampliada en pasos pequeños, cada uno probado (el roadmap por fases).
* **TDD (*Test-Driven Development*):** 🔴 escribir un test de algo que no existe (falla) → 🟢 código mínimo para que pase → 🔵 refactor sin cambiar comportamiento (los tests vigilan). Repetir.
* **Cambios pequeños:** commits y Pull Requests que hacen una sola cosa (una PR de 50 líneas se revisa; una de 3.000, no).
* **Interfaces primero:** decidir cómo encajan las piezas permite construirlas y probarlas por separado (o en paralelo, por personas distintas).
* **Separar responsabilidades:** decoder vs printer, `btcore` vs ejecutable.
* En una entrevista, "¿cómo abordarías un problema grande?" → descomponer hasta que cada trozo sea pequeño, comprensible y comprobable.

### 5. Interfaz pública vs interna
* **Pública** (el header; la usan el programa y los tests): una sola función "bytes → `Value`". Se decide **ya**, porque no cambia al añadir tipos: `decode("i42e")` y `decode("4:spam")` usan la misma función, así que los tests de los primeros pasos siguen valiendo al final.
* **Interna** (`bencode.cpp`, privada): funciones auxiliares ("leer entero", "leer string") y cómo llevan la posición. Se construye y reorganiza por pasos; los tests sobre la interfaz pública avisan si un refactor rompe algo.

### 6. Decisiones de la firma
* **Entrada: `std::string_view`.** Vista sin copias y de solo lectura del buffer que posee quien llama; permite `decode("i42e")` en los tests.
  * Alternativa vista en otros proyectos: `std::span<const std::byte>` (C++20), más explícito para datos binarios pero incómodo (no acepta literales, comparar con `'i'` requiere conversiones). Para bencode, cuya estructura es ASCII, `string_view` es lo práctico.
* **El resultado no depende del buffer:** `Value` guarda los strings como `std::string` (dueños), así que el decoder los **copia**. Tras `decode`, el buffer puede destruirse sin *dangling*. Coste: copiar (p. ej. los 26800 bytes de `pieces`). Alternativa: guardar `string_view` (cero copias, pero el árbol solo vale mientras viva el buffer). Para un `.torrent` de pocos KB, copiar es lo sensato.
* **Salida: devolver el `Value`, no un parámetro de salida.**
  * Estilo C: `bool decode(std::string_view input, Value& out);`. Existía para evitar copias (pre-C++11) y para devolver a la vez el resultado y si hubo error.
  * Estilo moderno: `Value decode(std::string_view input);`. Sin coste gracias a:
    * **RVO** (*Return Value Optimization*): el compilador construye el resultado directamente en la variable de quien llama (garantizado por el estándar en muchos casos desde C++17).
    * **Move semantics** (C++11): si no hay RVO, el objeto se **mueve**; mover un `std::vector`/`std::string` solo traspasa su puntero interno, sin copiar elementos.
  * Ventajas: se lee mejor (`auto torrent = decode(bytes);`), no hay `Value` a medio rellenar si algo falla, se puede encadenar (`find(decode(bytes), "info")`). Lo recomiendan las C++ Core Guidelines.
* **Pendiente: cómo informar de errores** si la función solo devuelve un `Value`:
  1. Lanzar una excepción.
  2. `std::optional<Value>`: un valor o nada (no dice **por qué** falló).
  3. Un tipo "resultado o error" (`std::expected` es C++23; el proyecto usa C++20).

### 7. `char` es un número (y puede ser negativo)
* `char` es un **tipo entero de 1 byte**; no existe un "tipo carácter" aparte. `'a'` es el número 97. Que se vea como letra o número depende del uso: `std::cout << c` lo interpreta como carácter; `int n = c;`, `c + 1` o `c == 'a'` lo usan como número.
* **Con o sin signo:** `unsigned char` va de 0 a 255; `signed char` de -128 a 127. El `char` a secas es **una u otra según la plataforma**: con signo en GCC/x86 (este proyecto), normalmente sin signo en ARM.
* **Bytes ≥ 128 se ven como negativos** en un `char` con signo. El byte en memoria es idéntico; cambia la interpretación:

| Byte | Como `unsigned char` | Como `char` (GCC x86) |
|---|---|---|
| `0x41` (`A`) | 65 | 65 |
| `0x7F` | 127 | 127 |
| `0x80` | 128 | -128 |
| `0xBE` | 190 | -66 |
| `0xFF` | 255 | -1 |

* Aparecen en los hashes de `pieces`, en los mensajes binarios de los peers y en los datos de las piezas.
* **Cuándo es un problema** (al usar el `char` como número):
  1. **Funciones que esperan `int`**: `std::isdigit(c)` solo acepta 0-255 o `EOF`; con `-66` es **comportamiento indefinido** (muchas implementaciones indexan una tabla → `tabla[-66]`).
  2. **Índices de arrays**: `conteo[c]++` con `c` negativo escribe fuera del array.
  3. **Montar enteros a partir de bytes** (longitudes big-endian del protocolo de peers): con `<<`, un byte ≥ 128 se promociona a un `int` negativo y la **extensión de signo** contamina los bits altos → una longitud de 200 puede salir negativa o enorme. Bug clásico en clientes de red.
* **No es problema** comparar con caracteres ASCII: `c >= '0' && c <= '9'`, `c == 'i'` (todos positivos; un byte negativo nunca coincide). Detectar dígitos así es seguro.
* **Protección:** antes de usar un byte como número, `static_cast<unsigned char>(c)` (p. ej. `std::isdigit(static_cast<unsigned char>(c))`). `-Wsign-conversion` y UBSan ayudan a detectar estos casos.

### 8. Tipos para datos binarios
* Para datos **puramente binarios** (hashes, mensajes de peers, contenido de piezas): **`std::uint8_t`** en lugar de `char`.
  * Siempre de 0 a 255 en cualquier plataforma: sin problemas de signo ni de extensión de signo.
  * Comunica "esto es un byte", no texto.
  * Los desplazamientos funcionan como se espera al montar enteros big-endian.
* **Frontera:** bencode sigue con `char`/`std::string_view` (estructura ASCII) y `pieces` queda en el `Value` como `std::string` con los bytes tal cual. Al extraer los hashes para usarlos, se convierten a `std::array<std::uint8_t, 20>`, y a partir de ahí el código de piezas y red trabaja con `std::uint8_t`.
* **Trampa:** `std::uint8_t` es un alias de `unsigned char` y `std::cout` lo imprime como **carácter** (`65` → `A`). Para depurar: `static_cast<int>(b)` o hexadecimal.
* **Alternativa estricta: `std::byte` (C++17).** Byte "puro": no permite aritmética, solo operaciones de bits; su valor se obtiene con `std::to_integer<int>(b)`. Más seguro conceptualmente, más incómodo. `std::uint8_t` es la opción pragmática habitual en código de red.

| Datos | Tipo |
|---|---|
| Estructura bencode (texto ASCII) | `char`, `std::string_view` |
| Strings dentro de `Value` (incluido `pieces`) | `std::string` (bytes tal cual) |
| Un hash SHA-1 | `std::array<std::uint8_t, 20>` |
| Buffers binarios (mensajes, piezas) | `std::vector<std::uint8_t>` |
| Vista de un buffer binario | `std::span<const std::uint8_t>` |

### 9. Estrategia de errores: por qué no un "valor de error"
* **Valor centinela** (*sentinel value*): un valor especial que significa "error". Es lo que hace C (`fopen` → `NULL`, `read` → `-1`) y lo que hace `find` (`nullptr` = no encontrado).
* **Funciona en `find`** porque `nullptr` no es un puntero válido a ningún `Value`: no puede chocar con un resultado real.
* **No sirve para el decoder:**
  1. **Reservar un `Value` existente** (p. ej. `Value{-1}` o un string vacío): en bencode **todos son datos válidos** (`i-1e`, `0:`). No hay forma de distinguir un error de un dato real.
  2. **Añadir una alternativa `DecodeError` al `variant`:** no choca, pero:
     * "Error" pasa a ser un tipo de dato bencode y no lo es. Se propaga a **todo el árbol** (una lista podría contener un error), y todo el código que use `Value` (`find`, el printer, `==`, el lector del `.torrent`) tendría que contemplarlo aunque los datos ya sean válidos.
     * **Es fácil olvidarse de comprobarlo:** el compilador no obliga, y el fallo aparece más tarde y en otro sitio (el gran problema de los centinelas en C).
* **La intuición correcta:** "lo que devuelvo es *o* un resultado *o* un error". El fallo era **dónde** ponerlo: el "o" va **por fuera** de `Value`, no dentro:
  ```
  decode  →  [ un Value  |  un error ]
  ```
  Así `Value` sigue significando "dato bencode válido" en todo el programa.
* **Opciones:**
  1. **Excepción:** `Value decode(std::string_view)`; si falla, lanza.
  2. **`std::optional<Value>`:** un valor o nada. No dice **por qué** ni **dónde** falló (un parser necesita "longitud inválida en el byte 312").
  3. **Resultado o error:** tipo con un `Value` o un error con mensaje y posición. `std::expected` es C++23; en C++20 habría que escribirlo o usar una librería.
* **Decisión: excepciones.**
  * El decoder es **recursivo**: un error en un entero dentro de una lista dentro de un diccionario **sube solo** hasta `decode`. Con `optional`/resultado, cada llamada recursiva tendría que comprobar y propagar el error a mano.
  * Un archivo corrupto es una situación **excepcional**, justo para lo que existen.
  * GoogleTest tiene `EXPECT_THROW`.

### 10. Excepciones en C++
* **Idea:** "aquí ha pasado algo que no sé resolver; que se encargue alguien de más arriba". La función **lanza** un objeto que describe el problema y la ejecución **salta** al sitio que lo **captura**, saltándose todo lo que hay en medio.
```cpp
#include <stdexcept>

int dividir(int a, int b) {
    if (b == 0) {
        throw std::runtime_error("division by zero");   // lanzar
    }
    return a / b;
}

int main() {
    try {                                    // bloque que puede fallar
        int r = dividir(10, 0);
        std::cout << r << '\n';              // nunca se ejecuta
    } catch (const std::runtime_error& e) {  // qué hacer si falla
        std::cerr << "Error: " << e.what() << '\n';
    }
}
```
* `throw` crea el error y **termina la función en ese instante** (no devuelve nada). `try { }` marca el código que puede fallar. `catch (const T& e)` se ejecuta solo si se lanzó un `T`. `e.what()` devuelve el mensaje.

#### Cómo sube una excepción (comprobado compilando)
```cpp
void nivel3() { throw std::runtime_error("algo ha fallado"); }
void nivel2() { nivel3(); std::cout << "nivel2 sigue\n"; }
void nivel1() { nivel2(); std::cout << "nivel1 sigue\n"; }

int main() {
    try {
        nivel1();
        std::cout << "main sigue dentro del try\n";
    } catch (const std::exception& e) {
        std::cerr << "capturado: " << e.what() << '\n';
    }
    std::cout << "main continua despues del catch\n";
}
```
Salida:
```
capturado: algo ha fallado
main continua despues del catch
```
* Se salta **todo** el código desde el `throw` hasta el `catch`, en todas las funciones intermedias (ningún `"... sigue"` se imprime, tampoco el resto del `try`).
* `catch (const std::exception&)` captura un `std::runtime_error` porque hereda de él.
* **Después del `catch`, el programa sigue con normalidad**: la excepción ya está resuelta.
* `nivel1` y `nivel2` **no hacen nada especial**: ni comprueban ni propagan. Ideal para el decoder recursivo:
  ```
  decode
   └─ leer diccionario
       └─ leer lista
           └─ leer entero  →  throw "invalid integer"
  decode  ←──── salta directo ──┘
  ```

#### `throw` sin `try/catch`, y dónde va cada uno
* **Un `throw` no necesita un `try/catch` en la misma función** (lo habitual es que no lo tenga). La función que detecta el problema no sabe qué hacer con él, así que solo avisa.
* **Una excepción siempre sube.** El `try/catch` no hace que el código "pueda lanzar": es una **red** que se pone en un punto concreto para pararla ahí. `nivel1` y `nivel2`, sin `try`, también se vieron afectadas.
* **Condición:** el `throw` tiene que ocurrir **durante la ejecución** de algún bloque `try` (a cualquier profundidad) con un `catch` del tipo adecuado. No hace falta que esté escrito dentro del `try`: basta con que se ejecute mientras el `try` está activo (p. ej. `dividir` llamada desde el `try` de `main`).

| ¿Dónde está el `throw`? | ¿Qué pasa? |
|---|---|
| Durante un `try` con un `catch` que encaja | sale del `try`, ejecuta el `catch` y sigue después |
| Durante un `try` cuyo `catch` no encaja con el tipo | sigue subiendo hasta el siguiente `try` de más arriba |
| Sin ningún `try` que la capture en toda la cadena | el programa termina abruptamente (`std::terminate`) |

  Sin captura, el programa muestra algo como `terminate called after throwing an instance of 'std::runtime_error' what(): division by zero` y aborta.
* **Regla:** lanzar donde se **detecta** el problema; capturar donde se puede **hacer algo útil** con él (normalmente mucho más arriba). `main` suele tener un `try/catch` alrededor de todo como **última red**: si algo falla y nadie lo resolvió antes, al menos se muestra un mensaje claro.
* **Analogía de la empresa:** director → jefe → encargado → empleado. El empleado encuentra un problema que no sabe resolver, para y avisa hacia arriba (`throw`). El encargado y el jefe, sin protocolo, dejan su trabajo a medias y lo pasan. El director tiene un protocolo (`try/catch`), lo resuelve y la empresa sigue. Si nadie tiene protocolo, la empresa cierra (`std::terminate`).

#### Stack unwinding (RAII)
* Mientras la excepción sube, C++ **destruye todas las variables locales** de las funciones que abandona (*stack unwinding*): los `std::vector`/`std::string` a medio construir liberan su memoria. Es el mismo mecanismo que `SCOPED_TRACE` (su destructor se ejecuta al salir del bloque, de forma normal o por excepción). Gracias a RAII, las excepciones no provocan fugas en C++ moderno.
* Lo único que se ejecuta durante el salto son esos destructores.

#### Tipos de excepciones (`<stdexcept>`)
```
std::exception                ← base de todas; tiene what()
 ├─ std::logic_error          ← errores del programador (un bug)
 │   ├─ std::invalid_argument
 │   └─ std::out_of_range
 └─ std::runtime_error        ← errores de los datos o del entorno
```
* Un bencode corrupto es un error de **datos** → derivar de `std::runtime_error` una clase propia:
  ```cpp
  class DecodeError : public std::runtime_error {
  public:
      using std::runtime_error::runtime_error;   // reutiliza sus constructores
  };

  throw DecodeError("invalid integer");
  ```
  * Quien capture `std::exception` también la captura, y tiene `what()`.
  * Permite capturar **solo** los errores de decodificación y añadirles información (p. ej. la **posición** del fallo).

#### Reglas de uso
* **Capturar por referencia constante** (`catch (const DecodeError& e)`): por valor se copiaría y, con un tipo derivado, se "recortaría" (*slicing*) perdiendo información.
* **El orden de los `catch` importa:** se elige el primero que encaje → los tipos concretos antes que los generales (`DecodeError` antes que `std::exception`).
* **Coste:** si no se lanza, prácticamente **cero** (diseño "zero-cost"); lanzar es caro (decenas de microsegundos). → Solo para situaciones **excepcionales**, no para el flujo normal. Un `.torrent` corrupto es excepcional (lanza); que `find` no encuentre una clave es normal (devuelve `nullptr`).

#### En GoogleTest
```cpp
EXPECT_THROW(btclient::decode("i42"), btclient::DecodeError);   // espera que lance ese tipo
EXPECT_NO_THROW(btclient::decode("i42e"));                      // espera que no lance
```
`EXPECT_THROW` es una red que pone GoogleTest y que comprueba que la excepción sí llega.

| Concepto | Qué es |
|---|---|
| `throw X` | lanza `X` y abandona la función |
| `try { }` | bloque cuya ejecución se vigila |
| `catch (const T& e)` | captura errores de tipo `T` |
| `e.what()` | mensaje del error |
| *stack unwinding* | al subir, se destruyen las variables locales |
| `std::runtime_error` | base para errores de datos o del entorno |
| clase propia (`DecodeError`) | distinguir los errores propios y añadirles información |
| `EXPECT_THROW` / `EXPECT_NO_THROW` | probarlas en GoogleTest |

### 11. Firma decidida del decoder
```cpp
// Lanza DecodeError si la entrada no es bencode válido.
Value decode(std::string_view input);
```
* Entrada: `std::string_view` (vista sin copias, solo lectura).
* Salida: el `Value` por valor (RVO / move).
* Error: excepción `DecodeError` (derivada de `std::runtime_error`).
* El decoder solo tendrá `throw`; los `try/catch` los pondrán quienes lo usen (`main`, y GoogleTest con `EXPECT_THROW`).

### 12. Repaso de clases en C++
#### Qué es una clase
* Un **tipo propio** que agrupa **datos** (miembros) y **funciones** que trabajan con ellos (métodos):
  ```cpp
  class Contador {
  public:
      void incrementar() { valor_++; }      // método
      int valor() const { return valor_; }  // const: no modifica el objeto

  private:
      int valor_ = 0;                       // dato miembro
  };

  Contador c;
  c.incrementar();
  c.valor();   // 1
  ```
* El `const` final de un método significa "no modifica el objeto" (el mismo que lleva `operator==`).

#### `public` / `private`
* `public:` → usable desde fuera (`c.incrementar()`). `private:` → solo desde los métodos de la clase (`c.valor_` desde fuera es error). Protege los datos internos.

#### `class` vs `struct`
* Son casi lo mismo; solo cambia lo que ocurre **si no se escribe nada**:

| | `struct` | `class` |
|---|---|---|
| Miembros sin `public:`/`private:` | públicos | **privados** |
| Herencia sin `public`/`private` | pública | **privada** |

* Convención: `struct` para datos simples y todo público (`Value`); `class` cuando hay datos que proteger o comportamiento.

#### Constructores
* Método especial que se ejecuta al **crear** el objeto; se llama como la clase y no tiene tipo de retorno.
  ```cpp
  class Persona {
  public:
      Persona(std::string nombre) : nombre_(nombre) {}   // lista de inicialización
  private:
      std::string nombre_;
  };

  Persona p("Ana");
  ```
* `: nombre_(nombre)` es la **lista de inicialización**: inicializa los miembros antes de entrar en el cuerpo.

#### Herencia: "es un"
```cpp
class Animal {
public:
    std::string nombre() const { return "animal"; }
};

class Perro : public Animal {};   // un Perro ES UN Animal

Perro p;
p.nombre();   // "animal": Perro tiene todo lo de Animal
```
* La clase derivada hereda todo lo de la base y puede añadir lo suyo.
* **Herencia `public`:** todo el mundo sabe que un `Perro` es un `Animal` → se puede pasar donde se espera un `Animal&`, y un `catch (const std::exception&)` captura un `DecodeError`.
* **Herencia privada** (lo que pasa en una `class` si no se escribe `public`): `Perro` tiene lo de `Animal` por dentro, pero hacia fuera **no es** un `Animal`.

#### Heredar constructores con `using`
* Los constructores **no se heredan** automáticamente. Dos formas:
  ```cpp
  class Perro : public Animal {
  public:
      Perro(std::string nombre) : Animal(nombre) {}   // a mano: se lo pasa al de la base
  };

  class Perro : public Animal {
  public:
      using Animal::Animal;                           // C++11: hereda todos los de la base
  };
  ```

#### Aplicado a `DecodeError`
```cpp
#include <stdexcept>

// Thrown when the input is not valid bencode.
class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
```
* **Error del primer intento:** `class DecodeError : std::exception {};`
  1. **Herencia privada** (falta `public`): un `catch (const std::exception&)` **no la capturaría** → la red de `main` fallaría y el programa terminaría abruptamente.
  2. **`std::exception` no guarda un mensaje** (su `what()` es genérico). `std::runtime_error` guarda el mensaje del constructor y lo devuelve con `what()`, y es la categoría correcta (error de datos).
  3. **Sin constructor que acepte el mensaje**: `using std::runtime_error::runtime_error;` (en la sección `public:`, porque en una `class` todo es privado por defecto).
* Con esto: `throw DecodeError("invalid integer");` funciona, `e.what()` devuelve el mensaje y `catch (const std::exception&)` la captura.
* Comprobación rápida: `EXPECT_THROW(throw btclient::DecodeError("test"), std::exception);`.
* Dentro de la clase, `using runtime_error::runtime_error;` (sin `std::`) también vale: el nombre de la base es visible.
* **A futuro:** añadir un miembro con la **posición** del fallo ("invalid integer at byte 312").

#### Revisión del header
* Declarar `DecodeError` **antes** de `decode`: el tipo de error aparece antes que la función que lo lanza.
* **Documentar el contrato** de `decode`, sobre todo que lanza:
  ```cpp
  // Decodes a single bencoded value from `input`.
  // Throws DecodeError if `input` is not valid bencode.
  [[nodiscard]] Value decode(std::string_view input);
  ```
* `[[nodiscard]]` en `decode`: decodificar sin usar el resultado no tiene sentido.
* Sangría de `public:`: al mismo nivel que `class` (o con 1 espacio, estilo Google), y el contenido con 4 más.
* `decode` declarada pero sin implementar compila mientras nadie la llame (fallaría al enlazar al usarla).

### 13. Estado y siguientes pasos
1. ✅ Tests de `Value` (`find` e igualdad): 18 tests.
2. ✅ Firma del decoder: `std::string_view` → `Value`, errores con `DecodeError` (declarados en `bencode.h`).
3. ✅ Paso 1 del decoder: enteros correctos (`i42e`, `i0e`, `i-7e`), con los tests primero. *(Ver entrada siguiente.)*

---

## [2026-11-12] Decoder, paso 1: enteros correctos

### 1. La especificación de bencode
* **Oficial:** BEP 3, sección *"bencoding"* (`https://www.bittorrent.org/beps/bep_0003.html`). Muy corta, en lenguaje natural, sin gramática formal (no es como un RFC).
  * Strings: longitud en base 10 + `:` + bytes.
  * Enteros: `i` + número en base 10 + `e`; pueden ser negativos. **Sin límite de tamaño**. `i-0e` inválido. Ceros a la izquierda inválidos (`i03e`), salvo `i0e`.
  * Listas: `l` + elementos + `e`. Diccionarios: `d` + pares + `e`; claves **strings**, **ordenadas como strings en bruto** (bytes), no alfanuméricamente.
* **Huecos que decide cada implementación** (documentar las decisiones propias):
  * "Sin límite" en enteros → en la práctica 64 bits: `std::int64_t`; si no cabe, `DecodeError`.
  * Ceros a la izquierda en la **longitud** de un string (`04:spam`): no se menciona; muchas implementaciones lo rechazan (una sola codificación por valor).
  * Claves repetidas: no se dice explícitamente; "ordenadas" suele leerse como estrictamente ordenadas.
  * Límite de anidamiento: no se menciona; decisión de seguridad.
* **Otras fuentes:** *BitTorrentSpecification* de wiki.theory.org (no oficial, más detallada; si contradice al BEP, manda el BEP) y el código de clientes reales (decodificador de libtorrent, el motor de qBittorrent).
* Hábito: ante una duda, ir primero al BEP; apuntar qué dice y qué se ha decidido en lo que no dice.

### 2. Nombres de suites y tests
* **Una suite por tipo:** `DecodeInteger`, `DecodeString`, `DecodeList`, `DecodeDict`, y `Decode` para lo general (entrada vacía, basura al final, profundidad). Permite filtrar: `--gtest_filter='DecodeInteger.*'`.
* **Verbos:** `Decodes...` para lo que debe funcionar; `Rejects...` para lo que debe lanzar `DecodeError`.
* **Sin guiones bajos** en nombres de suites ni tests: la macro `TEST` genera una clase uniéndolos con `_` (`DecodeInteger_DecodesPositiveInteger_Test`), y podrían chocar. *PascalCase*.
* **Probar los bordes:** los fallos se esconden justo en el límite. Comprobar los dos lados de la frontera: `DecodesMaxInt64` (`i9223372036854775807e`) y `RejectsOverflow` (`i9223372036854775808e`). `std::numeric_limits<std::int64_t>::max()`/`min()` (`<limits>`) dan los extremos sin escribirlos a mano.
* El cero merece test propio (`DecodesZero`): es el caso especial de la regla de los ceros a la izquierda.

### 3. El rojo: error del enlazador
```
/usr/bin/x86_64-linux-gnu-ld.bfd: ... in function `DecodeInteger_DecodesPositiveInteger_Test::TestBody()':
undefined reference to `btclient::decode(std::basic_string_view<char, std::char_traits<char> >)'
collect2: error: ld returned 1 exit status
```
* Error del **enlazador** (`ld`, `collect2`), no del compilador: los tests **compilan** (sintaxis correcta, `decode` bien declarada en el header), pero al juntar los `.o` no existe el **código** de `decode`. Es el rojo esperado en TDD cuando se declara antes de implementar.
* `std::basic_string_view<char, std::char_traits<char>>` es el nombre completo de `std::string_view`.
* **Tests vacíos = verde falso:** un `TEST` sin comprobaciones pasa siempre. En TDD el test lleva su comprobación desde el principio.

### 4. Estructura del decoder: funciones auxiliares
```
decode(input)                ← pública: mira el primer byte y decide
 ├─ 'i'      → decode_integer
 ├─ '0'-'9'  → decode_string
 ├─ 'l'      → decode_list
 └─ 'd'      → decode_dict
```
* `decode` es la "centralita"; cada auxiliar sabe leer **un solo tipo** (pequeñas y fáciles de cambiar). En el paso 1 solo existe `decode_integer` y `decode` la llama directamente.
* **Auxiliares privadas:** solo en `bencode.cpp` (no en el header), dentro de un **namespace anónimo**. Los tests solo usan `decode`, así que reorganizarlas no rompe los tests.
* **Deben ir antes que `decode`** en el archivo: el compilador lee de arriba abajo y tiene que conocerlas antes de verlas usadas.
* **Posición compartida:** las auxiliares reciben `std::size_t& pos` (referencia **no** constante: la avanzan). Al entrar apunta al inicio del valor; al salir, justo después. Necesario para las listas (`li1ei2ee`: cada elemento empieza donde terminó el anterior) y para detectar basura al final (`pos == input.size()`). Añadirla ya, sabiendo que hará falta, evita rehacer la función en el paso 4.
* **Nombres:** `snake_case` como `find`/`decode` → `decode_integer` (o `parse_integer`).

### 5. Namespace anónimo y enlace
* Cada `.cpp` se compila **por separado** a un `.o`; el **enlazador** los junta. Por defecto, cada función definida en un `.cpp` es **visible para todos** (así `main.cpp` usa `find`).
* **Problema:** dos auxiliares con el mismo nombre en dos `.cpp` → `multiple definition of ...`, o comportamiento indefinido si las firmas difieren (violación de la **ODR**, *One Definition Rule*). Y ninguna debía ser pública.
* **Solución:** todo lo que va dentro de `namespace { ... }` es **privado de ese `.cpp`** (**enlace interno** frente a **externo**). Equivale al `static` de C (`static int f(...)`); en C++ se prefiere el namespace anónimo (vale para funciones, variables y clases).
* **Ventajas:** encapsulación (como `private:` a nivel de archivo) y el compilador avisa si una auxiliar queda sin usar (`-Wunused-function`) y optimiza mejor.
* Puede ir **dentro** de `btclient` para que las auxiliares usen `Value` sin `btclient::`.

### 6. Devolver `Value`, no `const Value`
* `find` devuelve `const Value*`: puntero a datos **de otro** (dentro del diccionario de quien llama) → el `const` protege de verdad.
* `decode_integer` devuelve un `Value` **nuevo y propio** → `const` no protege nada (se puede copiar a una variable normal) y **rompe la move semantics**: mover es "robar" los datos internos, lo que exige modificar el origen; un objeto `const` no se puede mover y se **copia**. En `decode_list` (`elements.push_back(decode_value(...))`) supondría copiar cada subárbol en cada nivel.
* **Regla:** devolver por valor → sin `const` (C++ Core Guidelines). Puntero/referencia a datos ajenos → `const` si no deben modificarse. Igual en parámetros: `const Value& dictionary` (no es tuyo) vs `std::size_t& pos` (se modifica a propósito). La pregunta de siempre: **¿de quién es y quién puede modificarlo?**

### 7. `std::size_t`
* Entero **sin signo** para **tamaños y posiciones** en memoria (`<cstddef>`). En 64 bits: 8 bytes, de 0 a 2⁶⁴ − 1. Lo devuelven `.size()`, `sizeof`, `.find()`; se usa en índices `[ ]`.
* Mejor que `int` (32 bits con signo, ~2.100 millones; un tamaño negativo no tiene sentido).
* **Trampas:**
  1. Restar por debajo de 0 **da la vuelta**: `std::size_t a = 0; a - 1` → 18446744073709551615. En un parser, `final - inicio` al revés da una longitud gigantesca → lectura fuera del buffer.
  2. Mezclarlo con `int` → `-Wsign-conversion` (error con `-Werror`).
  3. **`npos`:** `find` devuelve `std::string_view::npos` (el máximo de `std::size_t`) si **no encuentra**. Es un **valor centinela**, como el `nullptr` de `find`. **Comprobarlo siempre** antes de usar la posición.

| Expresión | Qué es | Tipo |
|---|---|---|
| `pos` | por dónde se va | `std::size_t` |
| `input.size()` | cuántos bytes hay | `std::size_t` |
| `input.find('e', pos)` | dónde está la `e` (o `npos`) | `std::size_t` |
| `input[pos]` | el carácter en esa posición | `char` (`const char&`) |
| `'i'`, `'e'`, `'-'`, `'0'` | caracteres literales | `char` |
| `input.substr(...)` | un trozo del input | `std::string_view` |
* `pos` **no es un carácter**, es una posición; el carácter es `input[pos]`. Comparar `char` con `char` (`input[pos] == 'i'`) es seguro.
* **Antes de `input[pos]`, comprobar `pos < input.size()`**: si no, se lee fuera del buffer (UB; ASan lo caza). Casos `""` e `"i"` para el paso 2.

### 8. Errores en las auxiliares
* Un entero inválido **no devuelve nada**: `throw DecodeError("unterminated integer");`. La función termina ahí; no hace falta `return` después (el compilador lo sabe y no avisa de *"control reaches end of non-void function"*).
* Mensajes concretos: `"unterminated integer"`, `"invalid integer: leading zero"`, `"negative zero is not allowed"`... Es lo que mostrará `e.what()`.

### 9. Includes: en el `.cpp` o en el `.h`
* **Cada include en el archivo que lo necesita.** `<charconv>` (`std::from_chars`) solo se usa en la implementación → `bencode.cpp`.
* El header solo incluye lo necesario para **entender sus declaraciones** (`<variant>`, `<string>`, `<vector>`, `<string_view>`, `<stdexcept>`).
* Todo lo incluido en un header **se arrastra** a cada archivo que lo incluye: compilación más lenta y detalles internos expuestos.

### 10. Aprender la API sin memorizarla
* Nadie se sabe todas las funciones de la librería estándar. La lógica es lo difícil; la sintaxis es **vocabulario** que se aprende usándolo.
* **Método:**
  1. Escribir primero la lógica en comentarios; luego traducir línea a línea ("¿cómo se busca un carácter en un `string_view`?" es fácil de buscar).
  2. **cppreference.com**: cada tipo tiene la lista de funciones miembro con una línea por función.
  3. **Autocompletado** de VS Code: `input.` muestra todas las funciones miembro.
  4. Preguntar "¿cómo se hace X en C++?": la lógica sigue siendo propia.
* **Ejercicio:** reescribir `decode_integer` desde cero mirando solo los pasos en comentarios.

#### Chuleta
**`std::string_view`:**

| Quiero... | Se escribe |
|---|---|
| cuántos caracteres | `input.size()` |
| ¿está vacío? | `input.empty()` |
| carácter en una posición | `input[pos]` |
| buscar un carácter desde una posición | `input.find('e', pos)` → posición o `npos` |
| un trozo sin copiar | `input.substr(inicio, cuántos)` (**cuántos**, no hasta dónde) |
| ¿empieza por algo? | `input.starts_with('i')` (C++20) |
| puntero al primer carácter | `input.data()` |

**`std::from_chars`** (`<charconv>`), texto a número:
```cpp
std::int64_t number{};
auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
// ec == std::errc{}  → todo bien
// ptr                → hasta dónde ha leído
```
* Recibe **dos punteros** (inicio y fin), no un `string_view`. Acepta el signo `-`. No lanza excepciones, no depende del *locale*. `auto [ptr, ec]` son *structured bindings*.

**`std::vector`:**

| Quiero... | Se escribe |
|---|---|
| añadir al final | `v.push_back(x)` / `v.emplace_back(...)` |
| cuántos elementos | `v.size()` |
| el último | `v.back()` |

### 11. `decode_integer`
```cpp
// Decodes an integer of the form i<digits>e starting at `pos`, which must
// point at the 'i'. On return, `pos` points just past the closing 'e'.
Value decode_integer(std::string_view input, std::size_t& pos) {
    ++pos;  // skip 'i'

    const std::size_t end = input.find('e', pos);
    if (end == std::string_view::npos) {
        throw DecodeError("unterminated integer");
    }

    const std::string_view digits = input.substr(pos, end - pos);
    std::int64_t number{};
    std::from_chars(digits.data(), digits.data() + digits.size(), number);

    pos = end + 1;  // continue after 'e'
    return Value{number};
}

Value decode(std::string_view input) {
    std::size_t pos = 0;
    return decode_integer(input, pos);
}
```
```
input:  i  4  2  e
index:  0  1  2  3
```
1. `++pos` → salta la `i` (pos = 1). Con `std::size_t`, `++pos` y `pos++` son iguales; por costumbre se prefiere la forma prefija (evita una copia con iteradores complejos).
2. `find('e', pos)` → 3.
3. Sin `e` → `npos` → `DecodeError`. Sin esta comprobación, `input.data() + npos` apuntaría absurdamente lejos (UB).
4. `substr(pos, end - pos)` → desde 1, 2 caracteres → `"42"`, sin copia.
5. `from_chars` → 42 (de momento se ignora lo que devuelve; en el paso 2 detectará `i4x2e` y overflow).
6. `pos = end + 1` → justo después de la `e` (4).
7. `return Value{number};`
* `number{}` inicializa a 0. El comentario de arriba documenta el **contrato** de `pos` (dónde está al entrar y al salir), que necesitarán las listas.

#### Fallos del primer intento sin mirar
1. **`substr(pos, end)`**: el segundo argumento es la **cantidad**, no la posición final → `"42e"`. Correcto: `substr(pos, end - pos)`.
2. **Variable sin usar:** `digits` calculada pero el `from_chars` usaba `input.data() + pos` / `+ end` (correcto, otra forma) → `-Wall` avisa y `-Werror` no compila. Elegir una de las dos.
3. **`std::from_chars(digits, number)`** no existe: necesita dos punteros.
4. **Sin comprobación de `npos`.**
5. **`pos` sin avanzar tras la `e`**: los tres tests pasaban igual (solo comprueban el `Value`), pero rompería las listas y la detección de basura al final. **Lección de TDD: que los tests pasen solo garantiza que no hay bugs en lo que comprueban.** Lo cazarán los tests de listas y de `i42eXYZ`.
6. Detalles: `const` en `end`/`digits`, comentarios en inglés explicando el porqué (`// skip 'i'`), sangría de 4.

### 12. Tests de errores: `EXPECT_THROW` y `[[nodiscard]]`
```cpp
TEST(DecodeInteger, RejectsMissingTerminator) {
    EXPECT_THROW(static_cast<void>(btclient::decode("i7")), btclient::DecodeError);
}
```
* **`btclient::DecodeError`**: el tipo está en el namespace, igual que `decode`.
* **Choque con `[[nodiscard]]`:** `EXPECT_THROW` llama a `decode` y descarta el `Value` (solo le importa si lanza) → *"ignoring return value of ... declared with attribute 'nodiscard' [-Werror=unused-result]"*. Es el `[[nodiscard]]` funcionando como debe.
* **Solución:** `static_cast<void>(...)` evalúa la expresión (`decode` se ejecuta y puede lanzar) y descarta el resultado de forma explícita. Usarlo en todos los `Rejects...`.

### 13. Paso 2: tests de enteros inválidos
| Test | Entrada | Regla |
|---|---|---|
| `RejectsMissingTerminator` | `i42` | falta la `e` (ya pasa, por el `npos`) |
| `RejectsEmptyInteger` | `ie` | sin dígitos |
| `RejectsLoneMinusSign` | `i-e` | `-` sin dígitos |
| `RejectsNonDigitCharacters` | `i4x2e` | caracteres no numéricos |
| `RejectsPlusSign` | `i+5e` | `+` no permitido |
| `RejectsNegativeZero` | `i-0e` | prohibido por el BEP 3 |
| `RejectsLeadingZeros` | `i03e` | ceros a la izquierda |
| `RejectsNegativeLeadingZeros` | `i-03e` | ídem con negativo |
| `RejectsOverflow` | `i9223372036854775808e` | no cabe en `std::int64_t` |
| `DecodesMaxInt64` | `i9223372036854775807e` | máximo válido |
| `DecodesMinInt64` | `i-9223372036854775808e` | mínimo válido |
* **Orden:** (1) `RejectsMissingTerminator`; (2) vacío, `-` solo, no dígitos, `+` y overflow → casi todos con lo que devuelve `from_chars` (`ec` y si `ptr` llegó al final); (3) los extremos válidos (deberían pasar ya); (4) `-0` y ceros a la izquierda → `from_chars` **los acepta** (`03` es 3): necesitan una comprobación propia de los primeros caracteres.
* Escribir los tests y ejecutarlos **antes** de tocar `decode_integer` para ver exactamente cuáles fallan.

### 14. Estado y siguientes pasos
1. ✅ Paso 1: `decode_integer` + `decode`; 3 tests `DecodeInteger` (21 en total) en verde.
2. ⬜ Paso 2: tests `Rejects...` y extremos, validaciones en `decode_integer`.