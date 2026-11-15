# Iluminación de personajes (T264) — sobreexposición en interiores, oscuro a plena luz

Resumen para fusionar / revisar el cambio. Los dos archivos tocados son los mismos de siempre:

```
runtime/nvn/vulkan/exec.c   ->  runtime/nvn/vulkan/exec.c
docs/dev/tasks.md           ->  docs/dev/tasks.md
```

## Qué cambia

### 1. `runtime/nvn/vulkan/exec.c`

**Corrección definitiva: `char_light_src` / `T-CHAR-LIGHT`.**

Se identifica el programa de fragmentos de iluminación diferida de personajes (`fs` invitado `72c2cb3f00`) por el
**hash del contenido de su código** (`e4e7bf3a80de7cf3`), no por dirección, igual que `bloom_prefilter_src()` /
`bloom_tent_src()`.

La versión anterior escalaba por `0.35` el **color final** del pase. Eso arreglaba el Corredor del Silencio (antorcha
azul, personaje a 255) pero oscurecía al personaje a plena luz: en el cañón del mundo abierto la cara bajaba de R 71
(juego intacto) a R 41.

Ahora se escala **sólo el recojo de luz local** (antorchas / luces puntuales):

- El programa pone a cero `r41/r42/r43` al principio del bucle de luces, acumula una luz puntual por iteración y
  **suma al final** la luz direccional del sol (`cb3[13]`, un `fma` por canal, pc invitado `1548`). Después convierte
  esa suma en el término de Lambert/1π (pc `1518`..`1598`).
- El parche inserta tres multiplicaciones sobre `r41/r42/r43` justo al principio de ese epílogo, en la etiqueta
  `case 5400` del `switch` que genera el traductor. El sol **no** se toca.
- Consecuencia: a plena luz la suma local es ~0, así que el personaje queda **igual que en el juego** (la captura sale
  idéntica byte a byte, mismo md5, a la del build sin corrección). Bajo las antorchas del Corredor la suma local es
  casi todo el color, así que el resultado es el de la corrección anterior.
- El factor pasa de `0.35` a `0.27` porque ya no se atenúa la parte solar. Ajuste sin recompilar:
  `XCDE_CHAR_LIGHT=<factor>` (`1` = valores originales). Se registra `[char] character local light scaled by 0.27`.
- El ancla es la etiqueta `case 5400` a propósito: los comentarios `// pc` que se ven en los volcados GLSL sólo
  existen cuando se compila con `LARECOMP_SAVE_MSL`, así que no sirven como ancla en el código que traduce el runtime.
- Si el ancla no aparece, no se toca nada y se vuelca el fuente real a `build/char_live_fs.glsl` para poder mirarlo.
  El código parcheado tiene su propia identidad de caché SPIR-V; ningún otro programa se modifica.

### 2. `docs/dev/tasks.md`

Entrada **T264** al final: reproducción, hipótesis descartadas, medidas del Corredor y la parte nueva del mundo
abierto con la explicación de qué registro se escala y por qué el sol queda intacto.

## Medidas

| captura | cara (RGB) | pelo (RGB) | px ≥250 en el frame |
|---|---|---|---|
| Corredor, juego intacto (`XCDE_CHAR_LIGHT=1`) | 255,·,· | 255,·,· | 14242 |
| Corredor, versión anterior (0.35 a todo el color) | 154,116,95 | 157,147,117 | 5800 |
| Corredor, versión T264 (0.27 sólo luz local) | 157,121,99 | 157,147,115 | 5982 |
| Mundo abierto, juego intacto | 71,58,57 | 81,76,62 | — |
| Mundo abierto, versión anterior (0.35 a todo el color) | 41,·,· | — | — |
| Mundo abierto, versión T264 | 71,58,57 (idéntico, md5 igual) | 81,76,62 | — |

El entorno no cambia en ninguna de las dos escenas (zonas sin personaje idénticas).

## Comprobación rápida

```
# Corredor del Silencio (guardado del Corredor en build/save_fix1)
DRIVE_SAVE=build/save_fix1 scripts/drive_env.sh tests/routes/load_a.txt 2200 220 prueba_cor

# Mundo abierto (guardado actual en save/)
DRIVE_SAVE=save scripts/drive_env.sh tests/routes/load_a.txt 3000 220 prueba_dia
```

`build/prueba_cor_2200.png` debe tener la cara en tono natural (≈157,121,99) y `build/prueba_dia_3000.png` debe ser
idéntica a la del juego sin corrección. En el log aparece una línea `[char] character local light scaled by 0.27`.
Con `XCDE_CHAR_LIGHT=1` se recupera el aspecto original para comparar A/B.

## Nota

El cambio es sólo en el runtime (ningún dato del juego ni binario), así que aplicarlo encima del repositorio no
elimina nada de lo que ya está.
