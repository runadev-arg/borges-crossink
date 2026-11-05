---
title: Pasar el X4 a CrossInk Borges
---

# Pasar el X4 a CrossInk Borges

Para un Xteink X4 con Borges 1.6.0-borges.1. Lo probado en esta PC está marcado
**[PC]**. Ningún paso se probó todavía en un lector real.

## Qué pasa con tus datos

El firmware viejo guarda todo en `/.borges`. El nuevo guarda en `/.crosspoint` y del
viejo sólo lee el vínculo con la cuenta. Nada se borra, pero el nuevo arranca así:

| Dato | Después de pasar |
|---|---|
| Vínculo con Borges | Sigue, sin volver a vincular |
| Libros y `/.fonts` | Siguen |
| Wi-Fi, idioma y ajustes | Vacíos, y la interfaz arranca en inglés |
| Libros recientes | Vacíos |
| Posición de lectura | En el lector se pierde; vuelve la que ya estaba en el servidor |
| Marcadores locales | No se ven, siguen en `/.borges/bookmarks` |
| Cola sin enviar | No se envía, sigue en `/.borges/borges/outbox` |
| Índices de los libros | Se rehacen al abrir cada libro: tarda la primera vez |

## Antes de empezar

1. Con el firmware viejo y Wi-Fi: **Ajustes > Sistema > Borges > Sincronizar ahora**. Seguí hasta
   que diga `Salida: 0`.
2. Copiá la carpeta `/.borges` de la tarjeta a la PC.
3. Usá el `.bin` de una release, no uno de desarrollo. Si su versión dice `-dev+`, es de
   desarrollo.

## Opcional: conservar Wi-Fi, idioma y posición

Hacelo con la tarjeta en la PC y **antes** del primer arranque del firmware nuevo.
Salió de leer el código de los dos firmwares; no se probó en un lector.

```powershell
$sd = 'E:'   # la letra de tu tarjeta
New-Item -ItemType Directory -Force "$sd\.crosspoint" | Out-Null
foreach ($f in 'settings.json','wifi.json','recent.json') {
  if (Test-Path "$sd\.borges\$f") { Copy-Item "$sd\.borges\$f" "$sd\.crosspoint\$f" }
}
Get-ChildItem "$sd\.borges" -Directory -Force | Where-Object { $_.Name -match '^(epub|xtc|txt)_' } | ForEach-Object {
  if (Test-Path "$($_.FullName)\progress.bin") {
    New-Item -ItemType Directory -Force "$sd\.crosspoint\$($_.Name)" | Out-Null
    Copy-Item "$($_.FullName)\progress.bin" "$sd\.crosspoint\$($_.Name)\progress.bin"
  }
}
```

## Camino 1: por tarjeta SD (sin PC conectada)

1. Copiá `firmware-x3-x4-v<versión>.bin` a la raíz de la tarjeta.
2. En el lector: **Ajustes > Sistema > SD Card Firmware Update**.
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

Todavía no. El firmware viejo sólo acepta descargas de `runadev-arg/borges-firmware` y
lo que anuncie el manifiesto de `borges.runadev.com`, y ninguno ofrece esta versión.

Una vez instalado, el firmware nuevo busca en **Ajustes > Sistema > Buscar actualización**
la última release de `runadev-arg/borges-crossink`. Necesita unos 6,5 MB libres en la
tarjeta.

## Volver al firmware viejo

1. Bajá `borges-x4.bin` de
   <https://github.com/runadev-arg/borges-firmware/releases/tag/v1.6.0-borges.1>.
   Su SHA-256 es `c6cf23ff8cd8510b369b3aecf3cb6e902b9a31afebc0ff399a6787c68480d4e1`.
2. Instalalo igual que antes: por SD con **Ajustes > Sistema > Actualizar firmware por
   tarjeta SD**, o por USB con `-Firmware borges-x4.bin`.

Vuelve con lo que tenía `/.borges`. Lo leído en el firmware nuevo queda en el servidor si
se sincronizó, y en `/.crosspoint` si no. No borres `/.borges` mientras quieras poder
volver: si falta, el firmware viejo renombra `/.crosspoint` a `/.borges` y no entiende
las contraseñas de Wi-Fi guardadas por el nuevo.

**[PC]** La imagen 1.6.0-borges.1 pasa el mismo validador, que es igual en el firmware
nuevo, y su SHA-256 coincide con la release publicada.

## Si no arranca

Probá el camino 2 con `borges-x4.bin`. Si el lector no aparece como puerto USB, consultá
`docs/troubleshooting.md`.
