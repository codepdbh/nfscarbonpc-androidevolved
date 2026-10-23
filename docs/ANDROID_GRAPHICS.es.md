# Opciones gráficas antes de iniciar

Resolución interna, calidad gráfica y límite de FPS aparecen primero en el launcher. Los cambios se aplican al volver a pulsar JUGAR, iniciando un proceso del juego nuevo.

- Resolución: nativa, porcentajes o alturas de 720, 480 y 360 píxeles. El ancho se calcula con la proporción de la pantalla y se redondea a un número par.
- Calidad ultra baja (gama baja, p. ej. Redmi Note 8): los ajustes de la baja y, además, resolución interna limitada a 360 líneas aunque el launcher pida más.
- Calidad baja: detalle reducido, sin sombras, reflejos, partículas, clima ni posprocesado.
- Calidad media: detalle intermedio y partículas; sin sombras, reflejos, clima ni posprocesado.
- Calidad alta: detalle alto, sombras, reflejos y efectos; antialiasing desactivado.
- Conservar ajustes del juego: no modifica la calidad guardada por NFSCO.

Los perfiles modifican las opciones gráficas de `NFSCO/registry.json` antes de `OnAttach` y activan los vídeos (`are_movies_enabled`). Conservan idioma, audio y otras opciones existentes. El guardado usa archivo temporal y renombrado.

El escritorio virtual, GetSystemMetrics, los monitores, las consultas de modo de Direct3D, la ventana NFSCO y WidescreenFix deben comunicar la misma resolución interna. Android conserva su superficie y controles a tamaño físico; la imagen final se amplía para presentarla.

## Validación en Redmi Note 8

Compilación debug ARM64 correcta. Instalado y comprobado el perfil bajo con 480p y límite de 30 FPS: backbuffer 1040×480, ventana 1040×480 y WidescreenFix 1040×480, todos con proporción 13:6. El JSON conserva vídeos desactivados e idioma español. El menú online muestra campos y botones dentro de la pantalla y el registro informa 30 FPS. Capturas y registros locales en `work/device/redmi_launcher_quality.png`, `redmi_quality_480.png` y `redmi_quality_480.log`.

Esto no confirma rendimiento en pista ni resuelve el atasco de carga de las carreras online; esa investigación continúa en `ONLINE_LOADING_DEBUG.md`.

## Cambiar el nivel de detalle dentro del juego

Al cambiar el nivel de detalle en Opciones > Vídeo el juego reinicia el dispositivo D3D9 y DXVK redimensiona la superficie Android a la resolución interna. La cadena de presentación debe usar ese mismo tamaño (`getWindowSize` en `ports/dxvk_android/android_wsi.cpp` de nfsu2-recompiled): antes seguía al tamaño de la pantalla y Android recortaba la esquina superior izquierda, con los menús ampliados 2,25× y fuera de la pantalla. Desde ahora la superficie se presenta siempre a la resolución interna y el compositor de Android la escala, lo que además ahorra la copia a pantalla completa en cada fotograma.

NFSCO centra su ventana con `GetClientRect(GetDesktopWindow())` y la mueve con `MoveWindow` tras cada reinicio; ambas llamadas siguen ahora el escritorio virtual (la resolución interna).

## Rendimiento medido en Redmi Note 8 (Snapdragon 665)

Menús a 30 FPS. Carrera rápida con 8 coches en ultra baja: unos 10 FPS. El hilo principal está limitado por CPU: código del juego, efectos D3DX y puente D3D9; el hilo de audio de EA (RWAudioCore) ocupa otro 20–25 % de CPU y comparte el turno del intérprete con el juego. Las llamadas indirectas del código traducido pasan por una caché directa antes de la tabla hash.

Pendiente: las luces de la ciudad (streak flares, `STREAKFLARES_I`, renderer en 0xB4CF28) se ven como líneas largas. Hipótesis sin confirmar: la estela se calcula con el movimiento entre dos fotogramas y con pocos FPS ese salto es mayor; falta compararlo con un dispositivo rápido.
