# senew-app-videos — Videos

*Español más abajo — [ir a la versión en español](#español).*

> [!WARNING]
> **senew is a new project; use it at your own risk.** The apps, the system and the tools are young software for a discontinued phone.
> Back up your phone before installing anything and expect rough edges.

Videos: search and watch videos on the phone through the SENUS companion service over the Bluetooth network bridge. The sound plays through the system music queue; the picture arrives as JPEG frames that the app decodes (with the small decoder in `src/img_jpeg.c`) and scales to the screen. Landscape mode plays full screen.

Needs SENUS open on the computer with its services enabled and the Bluetooth bridge connected (see the SENUS README).

This is an app (`.sen`) for **[senew](https://github.com/semakers/senew-w580)**, the open operating system for the Sony Ericsson W580i
(see <https://semakers.com>). It is written against the public API of **[senew-sdk](https://github.com/semakers/senew-sdk)**.

## Build

You need an `arm-none-eabi-gcc` toolchain (Arm GNU Toolchain 15.x tested), GNU make, Python 3.8+ and git.

```sh
git clone https://github.com/semakers/senew-app-videos
cd senew-app-videos
make              # -> build/videos.sen
```

The Makefile looks for the SDK next to this folder (`../senew-sdk`) and, if it is not there, clones it into `./.senew-sdk` the first time.
To use another copy: `make SENEW_SDK=/path/to/senew-sdk`. Everything about the toolchain and the API is in the SDK's documentation.

## Install

Open **SENUS** with the phone connected, go to **Apps** and use **"Subir .sen"** (upload a .sen) to put `build/videos.sen` on the phone.

## License

MIT — see [`LICENSE`](LICENSE). The comments in the sources are in Spanish (the project's working language).
Sony Ericsson, W580i and other names are trademarks of their respective owners and are used only to say which hardware this is about; this project is not affiliated with them.

---

<a id="español"></a>

# senew-app-videos — Videos

> [!WARNING]
> **senew es un proyecto nuevo; úsalo bajo tu propio riesgo.** Las apps, el sistema y las herramientas son software joven para un teléfono descatalogado.
> Haz una copia de seguridad del teléfono antes de instalar nada y cuenta con asperezas.

Videos: busca y ve videos en el teléfono mediante el servicio de SENUS, por el puente de red Bluetooth. El sonido lo toca el sistema por su cola de música; la imagen llega como cuadros JPEG que la app decodifica (con el decodificador pequeño de `src/img_jpeg.c`) y amplía a la pantalla. De lado se ve a pantalla completa.

Necesita SENUS abierta en el computador con sus servicios activados y el puente Bluetooth conectado (ver el README de SENUS).

Esta es una app (`.sen`) para **[senew](https://github.com/semakers/senew-w580)**, el sistema operativo abierto del Sony Ericsson W580i
(ver <https://semakers.com>). Está escrita contra la API pública de **[senew-sdk](https://github.com/semakers/senew-sdk)**.

## Compilar

Necesitas una cadena `arm-none-eabi-gcc` (probada la Arm GNU Toolchain 15.x), GNU make, Python 3.8+ y git.

```sh
git clone https://github.com/semakers/senew-app-videos
cd senew-app-videos
make              # -> build/videos.sen
```

El Makefile busca el SDK junto a esta carpeta (`../senew-sdk`) y, si no está, lo clona en `./.senew-sdk` la primera vez.
Para usar otra copia: `make SENEW_SDK=/ruta/a/senew-sdk`. Todo sobre la cadena de herramientas y la API está en la documentación del SDK.

## Instalar

Abre **SENUS** con el teléfono conectado, entra en **Apps** y usa **«Subir .sen»** para poner `build/videos.sen` en el teléfono.

## Licencia

MIT — ver [`LICENSE`](LICENSE). Los comentarios de los fuentes están en español (la lengua de trabajo del proyecto).
Sony Ericsson, W580i y otros nombres son marcas de sus respectivos dueños y se usan solo para decir de qué aparato se trata; este proyecto no tiene relación con ellos.
