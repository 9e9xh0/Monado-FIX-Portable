# Cambios para fusionar con el repositorio (T262 — iluminación de personajes)

Copia del árbol del proyecto con **solo los archivos modificados**, en la misma ruta relativa que tienen
en el repositorio. Para aplicarlos, copia el contenido de esta carpeta sobre la raíz del proyecto:

```
GITHUB/runtime/nvn/vulkan/exec.c   ->  runtime/nvn/vulkan/exec.c
GITHUB/docs/dev/tasks.md           ->  docs/dev/tasks.md
```

## Qué cambia

### 1. `runtime/nvn/vulkan/exec.c`

**a) Corrección: sobreexposición de personajes (`char_light_src`, T-CHAR-LIGHT).**

Nuevo bloque justo después de `bloom_tent_src()` y una llamada en `compile_program()`, al mismo nivel que
los reemplazos de bloom existentes:

- Se identifica el programa de fragmentos de iluminación diferida de personajes (`fs` invitado `72c2cb3f00`)
  por el **hash del contenido de su código** (`e4e7bf3a80de7cf3`), no por dirección, igual que
  `bloom_prefilter_src()` / `bloom_tent_src()`.
- Se inserta una línea antes de su epílogo que escala por `0.35` el color final (registros `r0`, `r1`, `r2`
  volcados a `c0`); `c1` no se toca. El código parcheado tiene su propia identidad de caché SPIR-V.
- Si el ancla del epílogo no aparece, no se toca nada (no rompe otras variantes ni otros programas).
- Ajuste sin recompilar: `XCDE_CHAR_LIGHT=<factor>` (`1` = valores originales, `0` = desactivado,
  por defecto `0.35`). Se registra una línea `[char] character lighting scaled by 0.35` al aplicarse.

**b) Volcados de depuración ampliados en el bloque `LARECOMP_DEBUG_DATA`.**

Dentro del `if (getenv("LARECOMP_DEBUG_DATA"))` existente, solo activo con esa variable de entorno:

- `cb1` (constantes del compilador incrustadas en el blob del shader, tanto `vs` como `fs`).
- `sbuf` (direcciones de storage buffers con las que el shader recorre memoria del invitado, `cb0[81..]`).
- `fcb*` ahora volca hasta 132 floats en vez de 96 (constantes completas de los pases de personaje).

No tienen efecto en una partida normal (todo está detrás de la variable de entorno) y se usaron para
descartar las hipótesis de bloom, gamma, albedo, LUT y estructura de luces.

### 2. `docs/dev/tasks.md`

Entrada **T262** al final, con la reproducción, las hipótesis descartadas, la medida (píxeles ≥250:
14242 antes / 1526 después / 1312 en la referencia; p99 209.6 frente a 206.4) y el pendiente de validar
el factor en escenas diurnas.

## Comprobación rápida

```
DRIVE_SAVE=save scripts/drive_env.sh tests/routes/load_a.txt 2200 180 prueba
```

debe dejar `build/prueba_2200.png` con la cara en tono natural (≈141,112,96) y una línea
`[char] character lighting scaled by 0.35` en el log. Con `XCDE_CHAR_LIGHT=1` se recupera el aspecto
original y se puede comparar A/B.

## Nota

La carpeta contiene únicamente los dos archivos de texto modificados; no incluye binarios, capturas ni
el resto del árbol, así que copiarla encima del repositorio no elimina nada de lo que ya está.
