# Opciones gráficas antes de iniciar

Resolución interna, calidad gráfica y límite de FPS aparecen primero en el launcher. Los cambios se aplican al volver a pulsar JUGAR, iniciando un proceso del juego nuevo.

- Resolución: nativa, porcentajes o alturas de 720, 480 y 360 píxeles. El ancho se calcula con la proporción de la pantalla y se redondea a un número par.
- Calidad baja: detalle reducido, sin sombras, reflejos, partículas, clima ni posprocesado.
- Calidad media: detalle intermedio y partículas; sin sombras, reflejos, clima ni posprocesado.
- Calidad alta: detalle alto, sombras, reflejos y efectos; antialiasing desactivado.
- Conservar ajustes del juego: no modifica la calidad guardada por NFSCO.

Los perfiles modifican las opciones gráficas de `NFSCO/registry.json` antes de `OnAttach`. Conservan idioma, vídeos, audio y otras opciones existentes. El guardado usa archivo temporal y renombrado.

El escritorio virtual, GetSystemMetrics, los monitores, las consultas de modo de Direct3D, la ventana NFSCO y WidescreenFix deben comunicar la misma resolución interna. Android conserva su superficie y controles a tamaño físico; la imagen final se amplía para presentarla.

## Validación en Redmi Note 8

Compilación debug ARM64 correcta. Instalado y comprobado el perfil bajo con 480p y límite de 30 FPS: backbuffer 1040×480, ventana 1040×480 y WidescreenFix 1040×480, todos con proporción 13:6. El JSON conserva vídeos desactivados e idioma español. El menú online muestra campos y botones dentro de la pantalla y el registro informa 30 FPS. Capturas y registros locales en `work/device/redmi_launcher_quality.png`, `redmi_quality_480.png` y `redmi_quality_480.log`.

Esto no confirma rendimiento en pista ni resuelve el atasco de carga de las carreras online; esa investigación continúa en `ONLINE_LOADING_DEBUG.md`.
