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
2. 🚧 `find()`: declarada con `[[nodiscard]]`, falta implementarla en `bencode.cpp` y probarla: buscar una clave que existe, una que no, y llamarla con un `Value` que no es diccionario.
3. ⬜ Empezar el decoder.