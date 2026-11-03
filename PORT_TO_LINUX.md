# PORT_TO_LINUX — Port de smo-recomp a Linux nativo (Arch + Wayland/niri, Vulkan, solo Linux)
ACTUALIZA ESTE MD cuando completes cada tarea
## Contexto

smo-recomp traduce el código AArch64 de *Super Mario Odyssey* 1.4.1 a C y lo enlaza con un runtime propio. Hoy todo
es Windows: 45 de los ~60 archivos de `runtime/` incluyen `windows.h`, la compilación produce `.obj/.dll/.exe` con
clang 9 para MSVC, y la ventana, el audio (waveOut), los vídeos (Media Foundation), las fuentes (GDI), la Ayuda
(WebView2) y los diagnósticos (SEH, `RtlVirtualUnwind`, `SuspendThread`) son APIs de Win32.

El usuario quiere un **ejecutable nativo de Linux** (decidido: **solo Linux**, sin mantener Windows), **juego completo**
y **Vulkan como único backend**. Máquina destino: Arch, Wayland + niri, i7-13620H (6 P-cores con SMT + 4 E-cores),
portátil híbrido RTX 4060 (nvidia-open, nvidia-prime) + Intel UHD.

Lo que NO cambia: el traductor (`tools/lifter/recomp.py`) genera C portable; `runtime.h`, `neon.h`, `fpexact.h`,
`mx_translate.c`, `tex_decode.c`, `nvn_layout.c`, `nvn_handles.c`, `clib.c` (printf con va_list AAPCS64) y el
desenrollado C++ (`nnunwind.c`, con setjmp/longjmp) ya son C estándar. El renderer Vulkan es el grueso del trabajo
gráfico y casi no toca Win32 salvo locks, eventos, contadores y la superficie.

## Modo de trabajo (pedido del usuario)

- **Todo dentro de la carpeta del repo.** Nada se escribe fuera: el dump va en `./dump` (lo copia el usuario; ya
  ignorado por git), la `.venv` en `./.venv`, compilación en `./build`, temporales en `./build/tmp` (no `/tmp` ni el
  scratchpad), locks de "places"/slots en `./build/run/`, cachés del driver y de shaders en `./build/cache`
  (`XDG_CACHE_HOME`, `__GL_SHADER_DISK_CACHE_PATH` apuntando ahí en `run.py`).
- **Paquetes del sistema** (requieren sudo, los instala el usuario con `! sudo pacman -S clang lld vulkan-headers`;
  ya están sdl3, shaderc, ffmpeg, freetype2, fontconfig, vulkan-icd-loader). Mientras falten, se avanza con
  comprobación de sintaxis con gcc.
- **Desarrollo continuo hasta las 03:00 (Perú)**, sin pausas para preguntar; orden por valor: Fases 0→1→2→3→4→5,
  luego 6 y 7. Commit al final de cada fase en la rama `linux-port`, para que nada se pierda si se agota el límite
  de uso; el plan y los commits permiten retomar con "continúa" después del reinicio.

**Leyenda:** `[x]` hecho · `[~]` en curso · `[ ]` pendiente.

**Seguimiento:** cada casilla se marca `[x]` cuando su parte está hecha; se incluye en el commit de cada fase.

## TODO

**Dump (2026-11-02):** `./dump/switch/smo141/{exefs,romfs,control.nacp,manual}` (`--dump ./dump/switch/smo141`),
sacado de `./dump/game.nsp` (base v0 + actualización v393216 = **1.4.1**) con `hactool`. La primera extracción del
usuario (`dump/switch/smo`, actualización v262144 = 1.3.0) tenía la RomFS dañada; el `main` es el mismo en ambas
(build id `b424be15…`), así que lo compilado sirvió sin cambios.

Cómo se extrajo (para repetirlo): el `hactool` de `build/tools/hactool` (SciresM, 2023) no acepta las actualizaciones
nuevas: (1) `prod.keys` trae claves `mariko_master_kek_source_*` de 34 dígitos que lo hacen abortar → copia filtrada;
(2) `title.keys` no tiene las del juego → se toman de los `.tik` del NSP (clave cifrada en 0x180, rights id en 0x2a0;
tickets comunes) en un `HOME` temporal con `.switch/{prod,title}.keys`, porque `--titlekey` solo vale para el NCA
principal y el base la busca en `title.keys`; (3) la sección BKTR nueva tiene 0x4200 bytes tras las tablas y su tabla
AES-CTR-EX termina en el inicio de la tabla de reubicación, no en el de subsecciones: copia parcheada en
`build/tmp/hactool-src` (acepta ambos; `HACTOOL_BKTR_LOOSE=1` salta la comprobación de tamaño). Validado: 4,666/4,666
`.szs` Yaz0 válidos, 246 archivos cambiados y 336 nuevos respecto a la base (`build/tmp/yaz0all.py`). Las copias de
claves y el NSP desempaquetado se borraron después.

- [x] Crear `PORT_TO_LINUX.md` y la rama `linux-port`
- [x] Fase 0 — `tools/target.py`: CLANG, extensiones `.o`/`.so`, nombres sin `.exe`
- [x] Fase 0 — `build_main.py`: `-fPIC -fvisibility=hidden -ffp-contract=off`, `.o`
- [x] Fase 0 — `build_exe.py`: `libsmo_game.so` + `smo_native` con pkg-config, rpath, shaderc enlazado
- [x] Fase 0 — `extract.py`: symlinks en vez de junctions
- [x] Fase 0 — `setup.py` deps (clang, lld, pkg-config) con mensajes de `pacman`
- [x] Fase 0 — `run.py` + tests: sin `.exe`, `flock`, PRIME, cachés en `./build/cache` (`run.py`,
  `check_game_places.py`, `difftest.py`, `linkcheck.py`, `check_all_shaders.py`, `bntx_layout_check.py`,
  `check_fast_paths.py`, `build_mx_equiv.py`, `stress_stages.py`, `run_cap_kingdom.py`; `tools/dev/cpuload.py` y
  `memmap.py` sobre `/proc`, `profnames.py` con `llvm-cxxfilt` del PATH)
- [x] Fase 0 — C de herramientas/tests sin `windows.h` (`difftest_host.c`, `linkcheck_stub.c`,
  `tests/{fast_paths,mx_equiv,mx_all}.c` con shaderc enlazado, `layout_probe.c` → `liblayout_probe.so`).
  Comprobado sin dump: `check_fast_paths.py` PASS; `mx_all`, `mx_equiv` y la sonda de texturas compilan y corren
- [x] Fase 1 — `runtime/core/plat.h` / `plat.c` (locks, eventos, atómicos, tiempo, hilos, memoria, archivos)
- [x] Fase 2 — `hostrt.c`: mapeo con mmap, manejador de señales e informe de crash
- [x] Fase 2 — `boot.c`, `sdklift_rt.c`, `gamedll.c`, `hitch.c`, `game_shim.c` (`visibility("default")`)
- [x] Fase 3 — `os_threads.c`: pthreads, topología P/E, slots con flock, prioridades (nice/SCHED_IDLE)
- [x] Fase 3 — `smo_nn.c` + `nvn_stub.c`: heap con memfd y vistas con mmap
- [x] Fase 3 — `nntime.c`, `nnmisc.c`, `nnsdk.c`
- [x] Fase 3 — `nnfs.c`, `nnfsdir.c` (POSIX, diario con fsync, rutas sin distinguir mayúsculas)
- [x] Fase 4 — `display.c`: ventana SDL3 (Wayland), eventos, reloj de 60 Hz
- [x] Fase 4 — `nvn_vk.c`: Vulkan por SDL, superficie, GPU discreta
- [x] Fase 4 — `nvn_vk_draw.c`, `nvn_vk_pipelines.c`, `nvn_vk_watchdog.c`, `nvn_misc.c` a `plat.h`
- [x] Fase 4 — `nvn_gl.c`: quitar el backend OpenGL, conservar el front end; `gfx_options.c`
- [x] Fase 4 — `vk_shaderc_shim.c` enlazado directo
- [x] Fase 4 — `nnhid.c` (teclado SDL con foco, sin biblioteca XInput; el pad XInput sintético de las pruebas se
  conserva en la frontera), `nnpad_sdl.c` (códigos `VK_*` → `vk_codes.h`)
- [x] Fase 5 — `smo_audio.c`, `nnaudio.c`: audio con un stream de SDL3
- [x] Fase 5 — `nnmovie_ff.c` (FFmpeg: libavformat desde memoria, NV12 por swscale, PCM por swresample);
  `nnmovie_mf.c` borrado; `nnmovie.c`
- [x] Fase 5 — `nnfontll.c` (FreeType, sin escalar ni hinting), `nnpl.c` (fontconfig de respaldo)
- [x] Fase 5 — `nnnet.c`, `nnsocket.inc`, `nnonline.inc` (sockets BSD, getrandom)
- [x] Fase 5 — `nnapplet.c` (zenity/kdialog, xdg-open), `smo_guide.c` (navegador), `nnalbum.c`, `nnservices.c`
- [x] Fase 5 — `runtime/game/*` (`smo_nerve.c` con `process_vm_readv`, `smo_fast.c` con `dl_iterate_phdr`,
  `smo_selftest.c`, `smo_ui.c`) y `libc/nncxxrt.c`
- [x] Compilación completa del runtime sin errores con clang (`build/tmp/syncheck.py`, sin dump: 50 de 50 archivos)
- [x] Enlace completo sin dump: `tests/check_link.py` compila el runtime con encabezados sustitutos, enlaza
  `smo_native` contra una `libsmo_game.so` vacía (0 símbolos sin resolver; encontró dos fallos: `#pragma comment(lib)`
  de Windows en `nnmisc.c`/`nvn_gl.c`, y la biblioteca del juego sin `soname`, con lo que `$ORIGIN` no servía) y
  corre `--shaderc-selftest` (OK) y `SMO_PAD_SELFTEST=1` (51/51 con mandos virtuales de SDL3)
- [ ] Limpieza — quitar el código del backend OpenGL de `nvn_gl.c` (hoy inalcanzable; sus 38 llamadas a GL 1.1
  enlazan contra `runtime/gfx/gl_absent.c`, que aborta si alguna se llamara) y luego borrar `gl_absent.c`. Son
  ~2500 líneas mezcladas con el front end: se hace **después de la Verificación 5**, para comparar antes/después
  con el juego corriendo
- [x] Fase 6 — barreras de memoria en `recomp.py`: `dmb`/`dsb` → `__atomic_thread_fence` (acquire con `LD`,
  release con `ST`, seq_cst el resto), `isb` → barrera del compilador, `ldar`/`ldaxr` → `lda<n>` (acquire),
  `stlr` → `stl<n>` (release) en `runtime.h`. Comprobado sin dump con `tests/check_ordered.py` (ELF falso; 256 casos
  iguales a Unicorn). Falta `difftest.py all` con el dump (va en la Verificación 2)
- [x] Fase 7 — `README.md` (Linux, estado: medido en Windows, aún no corrido en Linux), `docs/building.md`
  (pacman, `.venv`, PRIME, cachés, dump cifrado con hactoolnet), `docs/architecture.md` (plat.h, SDL3, memfd, FFmpeg,
  FreeType, sin OpenGL/WebView2/MF), `docs/debugging.md` (señales, `/proc`, nice/SCHED_IDLE, slots con flock),
  `.gitignore` (`*.so`, `smo_native`, `smo_test*`)
- [x] Verificación 1–2: `setup.py --dump ./dump/switch/smo --check`: compila 219/219, `difftest` 23,191/23,191
  idénticos, `linkcheck` 0 sin resolver, fn_table 54,214 MATCH
- [x] Verificación 3–4: `SMO_IMPORT_SELFTEST` 0 fallos (amiibo y Ayuda incluidos); `SMO_SDK_SELFTEST` igual que en
  Windows (los 2 fallos conocidos: errno, NumberLineAllocator); `check_all_shaders.py` 27,888/27,888;
  `bntx_layout_check.py` 20,203/20,203; `check_fast_paths.py` PASS
- [x] Verificación 5: arranque y juego con ventana (`run.py play --exe smo_native`, Joy-Con por SDL): RTX 4060,
  audio por PipeWire, 0 fallos de Vulkan. La primera vez el juego entrega ~28k programas de shaders en la pantalla de
  carga y se compilan todos (varios minutos); luego vienen de `build/cache`. Tres fallos de Linux encontrados y
  arreglados (2026-11-03), ver "Correcciones tras la primera partida"
- [x] Verificación 6: `tests/run_cap_kingdom.py` **PASS** (`capk3`): 14/14 puntos de control, 0 excepciones, 0 fallos
  de Vulkan, 0 underruns de audio, 59.7 fps de media (mínimo 53.9). Antes de las correcciones: FAIL por 156 / 47
  underruns (`capk1` en frío, `capk2` caliente)
- [ ] Verificación 7: stress de etapas
- [ ] Lanzador directo (carpeta con un binario/script que se lance sin `run.py`), pedido por el usuario

### Correcciones tras la primera partida (2026-11-03)

- **Juego colgado tras "Start" (4 de 10 arranques):** `HakoniwaStateDeleteSceneNrvDeleteScene` no terminaba nunca.
  `al::AsyncFunctorThread::start` [main 0x7aca60] envía el mensaje a su hilo y *después* borra la bandera de terminado;
  en la consola el hilo `DeleteScene` (17) no corre antes que el principal (16) en el mismo núcleo, en Linux CFS lo
  deja adelantarse al despertar y la bandera se borraba tras terminar. Arreglo en `os_threads.c`: prioridad 17 bajo
  `SCHED_BATCH` (un hilo BATCH no desaloja al despertar; sin privilegios). 20/20 + 10/10 arranques bien
  (`build/tmp/bootloop.sh`). `SMO_OS_BATCH=0` lo desactiva
- **Underruns de audio:** `smo_audio` (y los hilos de SDL que crea) heredaban el núcleo de consola del hilo invitado
  que lo arrancó (CPU 10) y competían con los hilos de carga del juego. Ahora `os_pin_host_thread()` al arrancar.
  Además, tras un parón el stream de SDL seguía con silencio y el colchón de 120 ms no volvía (305 underruns, uno por
  bloque, en `capk6`): `sink_submit` mete 80 ms de silencio en cada underrun y lo registra (`smo_audio: underrun N at
  frame F`). Resultado: `capk7`, `capk9`, `capk10`, `capk11` PASS con 0; `capk8` 1 underrun (el hilo de audio estuvo
  296 ms sin producir en la carga de Cap Kingdom; un hueco de 80 ms).
- **Pantallas de carga congeladas** (el juego se quedaba quieto y saltaba de golpe a la escena siguiente): el hilo
  principal esperaba hasta ~1 s en `nvnMemoryPoolMap` (`vp_note_use`, `runtime/gfx/nvn_stub.c`), que tomaba el lock
  exclusivo `g_vp_lock` en cada llamada. El hilo de inicialización de escena (prioridad 19 → SCHED_IDLE, mismo núcleo de
  consola que el principal) lo tomaba y quedaba desplazado por los hilos de carga: inversión de prioridad. Ahora es lock
  compartido (el exclusivo solo en `nvnMemoryPoolMapVirtual`, al arrancar) y `nobj` reserva/libera fuera del lock.
  Encontrado con la pila del hilo principal (`__pthread_rwlock_wrlock` ← `vp_note_use`); `SMO_HITCH` nombra ahora la
  función del juego también dentro de un import. `capk16`/`capk17` PASS: sin parones en la carga de Cap Kingdom (antes
  4–6 frames de 150–900 ms), peor ventana de 300 frames 58 fps (antes 28–44). Quedan 2 frames >100 ms por partida
  (frame 1445: render 80 ms; frame 5717: código del juego 140 ms al entrar en la torre).
  Pendiente: 18..31 bajo SCHED_IDLE puede dar la misma inversión con otros locks del runtime.
- **Cortinilla de cambio de escena transparente** (comparado con un vídeo del emulador: tras cerrarse la cortinilla
  con forma de gorra reaparecía la escena anterior ~1 s en vez de negro, y al salir de la Odyssey no había cortinilla):
  durante la carga el juego finaliza las texturas del layout de la cortinilla (256x256 y 512x512, formato 0x49) y las
  vuelve a crear en el mismo objeto y la misma memoria sin registrarlas otra vez; sigue dibujando con los huecos del
  pool antiguos, que en la consola siguen siendo válidos. El backend no ligaba nada (`slot N holds a DEAD texture` en
  el log de `SMO_VK_PASSSHOT`) y el negro salía transparente. Arreglo en `runtime/gfx/nvn_gl.c`: `tex_finalize` deja
  viva una textura que aún nombra alguna entrada de pool (se libera con el mecanismo normal cuando la última entrada
  se va) y `nvn_front_handle` usa la textura nueva del mismo objeto si es idéntica (misma memoria, formato, tamaño).
  Comprobado con `SMO_SHOT_RANGES` en la puerta de la torre (`capk23`): cierre, negro durante toda la carga y
  apertura sobre la escena nueva, como en el emulador. 0 imágenes "made again".
- Parón de audio intermitente en la carga de Cap Kingdom (frames ~1840–2100, 240–1370 ms sin producir, un hueco de
  80 ms): 4 de 9 ejecuciones (`capk20`, `capk22`–`capk24`), sin relación con los arreglos de arriba. Sin investigar.
- **Prioridades aplastadas:** `smo-spawner` nacía de un hilo invitado ya en nice 3 y núcleo 0; sin `RLIMIT_NICE` no
  podía volver a 0, así que los niveles 0..3 corrían todos en nice 3. Ahora se arranca en `os_apply_main_sched` antes
  de que el hilo principal tome su prioridad
- **Wayland:** SDL 3 elegía X11 en niri (le faltan fifo-v1 / commit-timing-v1); `display.c` pide `wayland,x11`
  (respeta `SDL_VIDEO_DRIVER` / `SMO_SDL_VIDEO`)
- **Sin sonido** en la primera partida: la salida por defecto de PipeWire era HDMI; el usuario la cambió (pavucontrol)


## Enfoque

Reemplazo directo de Win32 por POSIX + SDL3 + bibliotecas de Arch, con **un único encabezado pequeño**
`runtime/core/plat.h` (+ `plat.c`) para los patrones que se repiten en decenas de archivos, y conversión archivo por
archivo del resto. Sin capa de compatibilidad Win32 falsa.

Dependencias (paquetes de Arch): `clang lld llvm sdl3 vulkan-headers vulkan-icd-loader shaderc ffmpeg freetype2
fontconfig python` + `pip: lz4 capstone unicorn numpy Pillow` (en `.venv`).

---

## Fase 0 — Herramientas y compilación (Python)

- `tools/target.py`: añadir constantes `CLANG` (`$LLVM_ROOT/bin/clang` o `clang` del PATH), `OBJ_EXT='.o'`,
  `EXE='smo_native'`, `GAME_LIB='libsmo_game.so'`; todos los scripts las usan en lugar de `clang.exe`/`.obj`/`.dll`.
- `tools/lifter/build_main.py`: objetos `.o` con `-fPIC -fvisibility=hidden -ffp-contract=off` (mantener el límite de
  tiempo y `optnone` para funciones enormes; recalibrar `TIME_LIMIT` con clang moderno).
- `tools/lifter/build_exe.py`:
  - `libsmo_game.so` = objetos levantados + `game_shim.c` enlazado con `-shared -fuse-ld=lld` (solo `game_bind`
    visible). Se conserva la separación exe/biblioteca de `gamedll.c`/`game_shim.c` (cambiar `__declspec` por
    `__attribute__((visibility("default")))`).
  - `smo_native` = runtime + `gamedll.c`, `-Wl,-rpath,'$ORIGIN'`, enlazado con `pkg-config sdl3 vulkan shaderc
    libavcodec libavformat libavutil libswscale freetype2 fontconfig` + `-lpthread -ldl -lm`.
  - Quitar la DLL `smo_glslang`: `vk_shaderc_shim.c` se compila dentro del exe enlazando `libshaderc_shared`; el
    autotest de SPIR-V se mantiene (llamada directa en vez de `ctypes`).
  - Fuentes: quitar `nvn_gl.c`'s backend OpenGL (ver Fase 4), quitar `nnmovie_mf.c` y añadir `nnmovie_ff.c`.
  - Añadir `-ffp-contract=off` también al runtime (clang moderno contrae FMA con `-march=native`).
- `tools/extract/extract.py`: junction `mklink /J` → `os.symlink` (y su borrado seguro con `os.path.islink`).
- `scripts/setup.py` `check_deps`: comprobar `clang`, `ld.lld`, y cada `pkg-config --exists`; mensajes con el
  `pacman -S` correspondiente.
- `scripts/run.py`, `tests/*.py`, `tools/lifter/linkcheck.py`, `difftest.py`, `tests/check_fast_paths.py`: nombres
  sin `.exe`, copias del binario con `shutil.copy2` + `chmod`, "places" (mutex con nombre) → `fcntl.flock` sobre
  `build/run/place_<n>.lock`; variables PRIME por defecto (ver Fase 4).
- `tools/lifter/difftest_host.c`, `linkcheck_stub.c`, `tests/{fast_paths,mx_equiv,mx_all,layout_probe}.c`: quitar
  `windows.h` (timers → `clock_gettime`, VirtualAlloc → `mmap`).

**Control:** `setup.py --dump <dump>` llega hasta `compile`; `difftest.py all` = 23,194 de 23,194 con clang moderno.

## Fase 1 — `runtime/core/plat.h` / `plat.c` (base común)

Reemplazos únicos, usados por todos los archivos:
- Locks: `plat_lock` (mutex recursivo, reemplaza `CRITICAL_SECTION`), `plat_rwlock` (`SRWLOCK`), `plat_cond`
  (`CONDITION_VARIABLE`, con `plat_cond_wait_ms`), `plat_event` (evento manual/automático = mutex+cond,
  reemplaza `CreateEvent/SetEvent/WaitForSingleObject`).
- Atómicos: macros `plat_inc64/add64/xchg/cas` sobre `__atomic_*` (reemplazan `Interlocked*`).
- Tiempo: `plat_ticks()`/`plat_tick_hz()` (`CLOCK_MONOTONIC` en ns; reemplaza `QueryPerformance*`),
  `plat_sleep_ms`, `plat_sleep_until_ns` (`clock_nanosleep TIMER_ABSTIME`, reemplaza el waitable timer de alta
  resolución y `timeBeginPeriod`).
- Hilos: `plat_thread_create(fn, arg, stack_size)` (pthread con pila propia), `plat_tid()` (`gettid`),
  `plat_set_affinity(tid, mask)` (`sched_setaffinity`), `plat_set_thread_name`.
- Memoria: `plat_reserve/commit/protect/release` (`mmap PROT_NONE` + `mprotect`, `MAP_FIXED_NOREPLACE` para
  direcciones fijas), `plat_query(addr, &base, &size, &readable)` leyendo `/proc/self/maps` con caché (reemplaza
  `VirtualQuery` de los volcados de crash).
- Archivos: `plat_exe_dir()` (`/proc/self/exe`), `plat_mkdirs`, `plat_rename_replace` (`rename`),
  `plat_fsync`, `plat_rss_bytes()` (`/proc/self/statm`, reemplaza `K32GetProcessMemoryInfo`).
- `__declspec(thread)` → `_Thread_local`; `_aligned_malloc/_aligned_free` → `aligned_alloc/free`;
  `_stricmp` → `strcasecmp`; `malloc.h` → `stdlib.h`; rutas `\\` → `/`.

## Fase 2 — Núcleo (`runtime/core/`)

- `hostrt.c`: mapear imagen (0x7100000000), zona de imports y SDK (`sdklift_rt.c`) con `plat_reserve` fijo;
  protección de solo lectura con `mprotect`. Manejador de crash: `sigaction(SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT)`
  con `SA_SIGINFO|SA_ONSTACK` + `sigaltstack` por hilo; mismo informe (función invitada `c->fn`, registros, dirección
  fallida `si_addr`, RIP de `ucontext`, módulo por `dladdr`, pila del host con `backtrace()`). `RaiseException` de
  `guest_trap/guest_unimpl` → informe + `abort()`. Página vigilada (`watch_veh`) → SIGSEGV + `mprotect` + paso único
  con el flag TF de `EFLAGS` en `ucontext` y `SIGTRAP`.
- `boot.c`: hilo principal invitado con `plat_thread_create` (pila de 1 GB reservada), pila invitada y TLS con
  `mmap`; el muestreo del perfilador (`SuspendThread`/`GetThreadContext`) → `tgkill(tid, SIGPROF)` y captura de RIP en
  el manejador.
- `gamedll.c`/`game_shim.c`: solo cambia la visibilidad (constructor + `game_bind` igual).
- `hitch.c`: `GetSystemTimes` → `/proc/stat`; contexto de hilos vía la misma señal de muestreo.

## Fase 3 — Sistema operativo y archivos (`runtime/os/`, `runtime/fs/`)

- `os_threads.c`: objetos invitados sobre `plat_lock/plat_cond`; hilos con pthread y pila host grande.
  **Modelo de núcleos de la consola** (crítico: sin él 4 de 6 arranques fallaban):
  - Topología leída de `/sys/devices/system/cpu/cpu*/topology/{core_id,thread_siblings_list}` y
    `/sys/devices/cpu_core/cpus` (Intel híbrido): los 3 núcleos de consola = 3 **P-cores** físicos distintos (un hilo
    SMT cada uno, su hermano libre); el hilo de render = otro P-core; ayudantes del runtime = E-cores
    (`/sys/devices/cpu_atom/cpus`). `SMO_OS_HOST_CPUS` se conserva.
  - "Slots" entre juegos simultáneos: mutex con nombre → `flock` de `build/run/console_cores_<k>.lock`.
  - Prioridades: 6 niveles → `setpriority(PRIO_PROCESS, tid, nice)`; los niveles bajos (18..31) con `SCHED_IDLE`
    (separa 17 de 18 como exige el comentario de `os_threads.c:129`) y los altos con nice negativo solo si
    `RLIMIT_NICE` lo permite; `SMO_OS_RT=1` opcional: `SCHED_RR` vía rtkit/`CAP_SYS_NICE`. Medido con
    `tests/stress_stages.py` (Fase 6).
  - `GetCurrentProcessorNumber` → `sched_getcpu` traducido a núcleo de consola.
- `smo_nn.c`: el heap como sección de página compartida → `memfd_create` + `ftruncate` + `mmap(MAP_SHARED)`;
  `smo_heap_section_handle/offset` devuelven el fd; `nvn_stub.c` mapea las vistas virtuales de los pools con
  `mmap(fd, offset, MAP_FIXED)` y las libera con `munmap`. El "commit" del heap es implícito (memoria bajo demanda).
- `nntime.c`: zona horaria de `/etc/localtime`/`TZ` con la base tz pública ya soportada; reloj con `clock_gettime`.
- `nnmisc.c`: idioma de `LANG/LC_ALL`; `MessageBoxA` → `SDL_ShowSimpleMessageBox`.
- `nnfs.c`/`nnfsdir.c`: `FindFirstFile` → `opendir/readdir/stat`, `CreateDirectory` → `mkdir`,
  `MoveFileEx(REPLACE)` → `rename`, `FlushFileBuffers` → `fsync` (+ `fsync` del directorio en el commit del diario),
  `SetEndOfFile` → `ftruncate`; mutex del diario → `flock`; PID con `getpid`. **Búsqueda sin distinguir mayúsculas
  como respaldo** en la RomFS (Windows lo hacía implícitamente): si `open` da `ENOENT`, buscar cada componente con
  `strcasecmp` y cachear el resultado.

## Fase 4 — Gráficos, ventana y entrada (`runtime/gfx/`, `runtime/input/`)

- `display.c`: ventana con **SDL3** (Wayland nativo en niri; `SDL_WINDOW_VULKAN | RESIZABLE`, borderless/fullscreen
  con `SDL_SetWindowFullscreen`, F11/doble clic, foco y minimizado por eventos SDL). El bucle de eventos SDL vive en
  el hilo principal del host (requisito de SDL) y el reloj de 60 Hz usa `plat_sleep_until_ns`. Captura F12 igual.
- `nvn_vk.c`: cargar Vulkan con `SDL_Vulkan_GetVkGetInstanceProcAddr` (en vez de `vulkan-1.dll`); extensiones de
  instancia de `SDL_Vulkan_GetInstanceExtensions`; superficie con `SDL_Vulkan_CreateSurface`; tamaño con
  `SDL_GetWindowSizeInPixels`. **Elegir la GPU discreta** (`VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`, variable
  `SMO_VK_DEVICE` para forzar) y en `run.py` exportar `__NV_PRIME_RENDER_OFFLOAD=1` y
  `__VK_LAYER_NV_optimus=NVIDIA_only`. Caché de pipelines del driver: `__GL_SHADER_DISK_CACHE_PATH` → carpeta del
  juego (igual que el commit 91f048a).
- `nvn_vk_draw.c`, `nvn_vk_pipelines.c`, `nvn_stub.c`, `nvn_misc.c`: sustitución mecánica por `plat.h` (locks,
  eventos, atómicos, ticks, RSS). `nvn_vk_watchdog.c`: lista de hilos de `/proc/self/task/*/{comm,stat}`, propietario
  de locks registrado por `plat_lock` (tid), hora local con `localtime_r`.
- `nvn_gl.c`: es el **front end** común (listas de comandos, hilo de render) y además el backend OpenGL. Quitar todo
  lo de WGL/OpenGL (contextos, `glDraw*`, `GL/gl.h`, `renderer = opengl` en `gfx_options.c`) dejando el front end; el
  volcado de shaders/caché con `plat_*`.
- `vk_shaderc_shim.c`: enlace directo a shaderc del sistema.
- `nnhid.c`: teclado por eventos/estado SDL (`SDL_GetKeyboardState`, solo con foco) en lugar de `GetAsyncKeyState`;
  quitar el backend XInput y su pad sintético → el pad sintético de las pruebas (`SMO_XINPUT_SYNTH`) pasa a un mando
  virtual de SDL (`SDL_AttachVirtualJoystick`), que `nnpad_sdl.c` ya sabe tratar. `nnpad_sdl.c`: solo rutas y
  `_stricmp`.

## Fase 5 — Audio y servicios (`runtime/audio/`, `runtime/services/`, `runtime/game/`)

- `smo_audio.c`: `waveOut` → `SDL_OpenAudioDeviceStream` 48 kHz s16 estéreo (PipeWire), misma mezcla 6→2 y bloques
  de 40 ms, contador de huecos/underruns mantenido.
- `nnmovie_ff.c` (nuevo, sustituye `nnmovie_mf.c` con la misma interfaz que usa `nnmovie.c`): FFmpeg
  `libavformat`+`libavcodec` (H.264) + `libswscale` al formato que espera el reproductor, y el audio del vídeo si lo
  hay. Respetar `SMO_SKIP_MOVIES`.
- `nnfontll.c`: contornos con **FreeType** (`FT_Load_Glyph` sin hinting, `FT_Outline_Decompose`) en lugar de GDI
  `GetGlyphOutlineW`; la rasterización propia del archivo no cambia. `nnpl.c`: fuentes de respaldo de
  `deps/fonts`, luego fontconfig (Noto Sans, Noto Sans CJK) en lugar de `GetWindowsDirectory`.
- `nnnet.c`/`nnsocket.inc`: Winsock → sockets BSD (`errno`, `close`, `fcntl O_NONBLOCK`), `CryptGenRandom` →
  `getrandom`.
- `nnapplet.c`: teclado de software (`DialogBoxIndirectParamW`) → `zenity --entry` / `kdialog` si existen, si no
  el texto por defecto; páginas web `ShellExecute` → `xdg-open`; cadenas UTF-16 con conversión propia (no
  `wchar_t`, que en Linux es de 32 bits).
- `smo_guide.c`: la Ayuda se abre en el navegador con `xdg-open` (el respaldo que ya existe); se quita WebView2.
  Diferencia aceptada: el juego no se pausa mientras está abierta (se deja documentado).
- `nnalbum.c`, `smo_amiibo.c`, `smo_ui.c`, `smo_nerve.c`, `smo_selftest.c`, `smo_fast.c`, `gfx_options.c`: rutas,
  `localtime_r`, `plat_*`.

## Fase 6 — Corrección pendiente del traductor (recomendada)

Con clang moderno el optimizador es más agresivo: en `recomp.py` emitir `__atomic_thread_fence(__ATOMIC_SEQ_CST)`
para `dmb`/`dsb`, `__atomic_load_n(..., __ATOMIC_ACQUIRE)` para `ldar`/`ldaxr` y `__atomic_store_n(...,
__ATOMIC_RELEASE)` para `stlr`. Verificar con `difftest.py` (no cambia la semántica de un solo hilo).

## Fase 7 — Documentación

`README.md` (requisitos de Arch, estado "Linux"), `docs/building.md` (paquetes `pacman`, `.venv`, PRIME, Wayland),
`docs/architecture.md` (SDL3, memfd, FFmpeg, FreeType, quitar OpenGL/WebView2/MF), `docs/debugging.md` (señales en
vez de SEH, `/proc` en vez de Toolhelp). `.gitignore`: añadir `*.so`, `smo_native`, `smo_test*`.

---

## Verificación (en esta máquina, con el dump 1.4.1 en `./dump`)

1. `sudo pacman -S clang lld vulkan-headers` (lo ejecuta el usuario) y `./.venv` con los paquetes de pip.
2. `python scripts/setup.py --dump ./dump --check`: importar, funciones (54,214), traducir, compilar, enlazar, y
   `difftest` 23,194/23,194 + `linkcheck`.
3. Autotests del runtime: `SMO_SDK_SELFTEST=1`, `SMO_IMPORT_SELFTEST=1` (0 fallos), autotest de SPIR-V del enlace.
4. `python tests/check_all_shaders.py` (27,888/27,888), `tests/bntx_layout_check.py` (20,203), `check_fast_paths.py`.
5. `python scripts/run.py play --exe smo_native --seconds 0`: ventana en niri, GPU NVIDIA en el log
   (`nvn_vk: device ...`), título, selección de archivo, Resume, sonido, mando y teclado, vídeo del tutorial, F12.
6. `python tests/run_cap_kingdom.py`: 13 de 13 puntos de control, 0 excepciones del host, 0 fallos de Vulkan.
7. `python tests/stress_stages.py` (al menos un subconjunto de etapas) para validar el modelo de núcleos y
   prioridades en Linux; si aparecen los fallos de heap de `os_threads.c:141`, probar `SMO_OS_RT=1`.

## Riesgos

- **Planificación de hilos:** Linux no tiene prioridades estrictas sin privilegios; `SCHED_IDLE` + nice debería
  bastar, pero se mide en el paso 7.
- **Mayúsculas en rutas** de la RomFS: cubierto con el respaldo sin distinción en `nnfs.c`.
- **Volumen:** ~40 archivos del runtime tocados; se hace por fases y cada fase compila antes de seguir. El arranque
  real del juego solo se puede comprobar al final de la Fase 4.
