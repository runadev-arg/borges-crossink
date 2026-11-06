---
title: Pasar el X4 a CrossInk Borges
---

# Pasar el X4 a CrossInk Borges

Para un Xteink X4 con Borges 1.6.0-borges.1. Lo probado en esta PC está marcado
**[PC]**. Ningún paso se probó todavía en un lector real.

## Qué pasa con tus datos

Al primer arranque, el firmware nuevo copia lo tuyo desde `/.borges` a `/.crosspoint`. No borra ni
cambia nada en `/.borges`, así que podés volver atrás. Si hay muchos libros, muestra «Un momento…».

| Dato | Después de pasar |
|---|---|
| Vínculo con Borges | Sigue, sin volver a vincular |
| Libros y `/.fonts` | Siguen |
| Wi-Fi, idioma y ajustes | Siguen |
| Libros recientes | Siguen |
| Posición de lectura | Sigue |
| Marcadores | Siguen, sin volver a publicar los que el servidor ya tenía |
| Eventos sin enviar del firmware viejo | **No se envían**: las posiciones se regeneran, las sesiones de esa cola se pierden |
| Índices de los libros | Se rehacen al abrir cada libro: tarda la primera vez |

La interfaz viene en seis idiomas: castellano, inglés, portugués de Brasil, francés, alemán e
italiano. Si el lector estaba en otro, queda en inglés.

Si después querés volver al firmware viejo, se puede, pero hay que vincular el lector otra vez: el
vínculo viejo estaba en la tarjeta en una forma que cualquiera puede leer, así que este firmware lo
cambia por uno nuevo en la primera sincronización.

## Antes de empezar

1. Con el firmware viejo y Wi-Fi: **Ajustes > Sistema > Borges > Sincronizar ahora**. Seguí hasta
   que diga `Salida: 0`.
2. Copiá la carpeta `/.borges` de la tarjeta a la PC. Es tu copia de seguridad.
3. Usá el `.bin` de la release, no uno de desarrollo. Si su versión dice `-dev+`, es de
   desarrollo.

## Camino 1: por tarjeta SD (sin PC conectada)

1. Copiá `firmware-x3-x4-v<versión>.bin` a la raíz de la tarjeta.
2. En el lector: **Ajustes > Sistema > Actualizar firmware por tarjeta SD** (si está en inglés,
   *SD Card Firmware Update*).
3. Elegí el archivo y confirmá. Al terminar, reinicia solo.

Si el menú no abre: apagado, mantené **Arriba** y apretá **Encendido** para ir directo
al selector.

**[PC]** La imagen 1.6.1-borges.1 pasa una copia en Python del validador del firmware
viejo: chip, placa `x4`, tamaño, checksum y SHA-256.

## Camino 2: por USB

1. Prendé el lector y conectalo con un cable de datos.
2. Probá sin escribir nada:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\flash-x4.ps1 -Firmware <archivo.bin> -DryRun
   ```

3. Sacá `-DryRun` y escribí `yes` cuando pregunte.

El script escribe `0x10000` (app0) y `0xE000` (otadata). No toca bootloader, tabla de
particiones, NVS ni la tarjeta. Nunca uses `erase-flash`: borra la NVS, donde el
fabricante guarda datos propios del aparato.

No alcanza con grabar sólo `0x10000`: si el lector venía corriendo desde app1, seguiría
arrancando el firmware viejo.

**[PC]** Probado sin aparato: el modo de prueba y cada rechazo (otro chip, otra placa,
otro tamaño de flash, imagen muy grande, corrupta o cortada, y sin lector conectado).
Falta la escritura real.

## Camino 3: desde el navegador

No hay. `site/` sólo enlaza a Inky (`inky.crossink.dev`), que baja las versiones del
catálogo de CrossInk. Borges no está ahí. Usá el camino 2.

## Camino 4: actualización por Wi-Fi

Desde el firmware viejo, no: sólo acepta descargas de `runadev-arg/borges-firmware` y lo que
anuncie el manifiesto de `borges.runadev.com`, y ninguno ofrece esta versión.

Una vez instalado, el firmware nuevo busca en **Ajustes > Sistema > Buscar actualización**
la última release de `runadev-arg/borges-crossink`. Necesita unos 6,5 MB libres en la
tarjeta.

## Volver al firmware viejo

1. Bajá `borges-x4.bin` de
   <https://github.com/runadev-arg/borges-firmware/releases/tag/v1.6.0-borges.1>.
   Su SHA-256 es `c6cf23ff8cd8510b369b3aecf3cb6e902b9a31afebc0ff399a6787c68480d4e1`.
2. Instalalo igual que antes: por SD con **Ajustes > Sistema > Actualizar firmware por
   tarjeta SD**, o por USB con `-Firmware borges-x4.bin`.

Vuelve con lo que tenía `/.borges`, sin el vínculo. Lo leído en el firmware nuevo queda en el servidor si
se sincronizó, y en `/.crosspoint` si no. No borres `/.borges` mientras quieras poder
volver: si falta, el firmware viejo renombra `/.crosspoint` a `/.borges` y no entiende
las contraseñas de Wi-Fi guardadas por el nuevo.

**[PC]** La imagen 1.6.0-borges.1 pasa el mismo validador, que es igual en el firmware
nuevo, y su SHA-256 coincide con la release publicada.

## Si no arranca

Probá el camino 2 con `borges-x4.bin`. Si el lector no aparece como puerto USB, consultá
`docs/troubleshooting.md`.
