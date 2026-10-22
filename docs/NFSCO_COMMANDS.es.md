# Comandos NFSCO y propuesta para Android

Investigación: 22 de octubre de 2026. No se enviaron comandos a un servidor ni se crearon salas durante esta investigación.

## Evidencia

- [FAQ oficial](https://github.com/NFSCO/documentation/wiki/Frequently-Asked-Questions), copia local de la wiki en `work/nfsco-wiki`, revisión `b29c57679a8fcb046fc91173b74c78467d6b7d37`.
- Mensajes de uso y descripciones extraídos de la copia local de `NFSCO/bin/palmont.dll`, SHA-256 `96fcb4745b04e55a6a431d92fc5dc8a1daf040dd2e9f6091f3cde30b7c7deda6`. Extracción de diagnóstico: `work/nfsco_strings.txt`.
- Revisión del menú existente en `GameActivity.java`, métodos `showNfscoCommands`, `askNfsco` y `sendNfscoLine`.

## Comandos identificados

| Comando | Función | Argumento |
| --- | --- | --- |
| `/search Lofty` | Buscar eventos | Nombre parcial o identificador |
| `/race tn.2.4` | Abrir una sala para un evento existente | ID de pista/evento |
| `/join 5` | Entrar en la sala anunciada en el chat | Número de sala |
| `/start` | Iniciar la sala propia | Ninguno |
| `/leave` | Abandonar la sala actual | Ninguno |
| `/clients` | Mostrar jugadores conectados | Ninguno |

Los primeros cuatro están explicados en la FAQ; los seis aparecen con descripciones en la DLL instalada. Esto confirma su presencia, pero no sustituye una prueba de cada acción en Android.

Conectarse primero. Buscar el evento, copiar su ID al crear la sala y comunicar el número de sala a los demás. Al iniciar, permanecer en el menú principal: la DLL anuncia cinco segundos de espera. La FAQ excluye duelos de cañón, pursuit tag y pursuit knockout del online.

`/race` abre eventos existentes. No encontré evidencia de comandos para generar trazados nuevos, editar checkpoints, ajustar vueltas, tráfico, IA o colisiones. Tampoco confirmé `/help`, `/races`, `/ready`, `/kick` ni `/invite`; no deben añadirse como si estuvieran disponibles.

## Mejoras propuestas para el port

1. Cambiar las etiquetas a «Crear sala: ID de pista» y «Unirse: número de sala». Mostrar ejemplos distintos y explicar dónde encontrar cada identificador.
2. Guardar pistas favoritas y recientes en Android. Un botón de favorito compondría `/race <ID>` sin reescribirlo.
3. Añadir un botón «Repetir última pista» que cree una nueva sala bajo demanda. No encadenar automáticamente creación e inicio.
4. Mantener separados buscar, crear, unirse, iniciar, salir y jugadores. Mostrar una ayuda breve sobre el requisito del menú principal.
5. Para un selector visual de pistas, obtener resultados reales del buscador o construir un catálogo desde los datos locales. La función actual solo envía texto: todavía no recibe resultados estructurados ni confirma que el servidor haya aceptado una orden.
6. Validar el número de sala como entero positivo y el ID de pista como texto sin saltos de línea, sin imponer un patrón no documentado. Evitar envíos simultáneos y cancelar eventos pendientes si se cierra la actividad.

El menú existente ya tiene los seis comandos. La principal mejora pendiente es convertir la búsqueda y los IDs en una selección de pistas fácil de usar; enviar texto no basta para saber si se creó una sala.

## Otras funciones

La FAQ describe el paseo libre desde Carrera → Continuar → Paseo libre, después de completar el tutorial desconectado.

Los [códigos oficiales](https://github.com/NFSCO/documentation/wiki/Cheat-Codes) se escriben en el menú principal, sin barra y fuera del chat: `BOOORING` desbloquea contenido hasta reiniciar, `HARDERPLEASE` aumenta la habilidad de la IA y `REALCHALLENGE` la aumenta más y desactiva SpeedBreaker. `IFIWERERICH` concede dinero y requiere otro alias para deshacer su efecto. No aplicarlos automáticamente ni mezclarlos con los comandos online.

## Validación pendiente

En una sesión de prueba acordada: comprobar búsqueda con varios resultados, creación de sala, entrada de un segundo jugador, inicio desde el menú y salida. Probar errores de ID desconocido y sala inexistente. No dar por completada la carrera online por el solo hecho de que NFSCO conecte.
