/* One line per native area file src/natives/<area>.c (see native.c). No include guard:
 * included repeatedly with different NATIVE_AREA definitions. */
NATIVE_AREA(test)            /* 3.0 framework test */
NATIVE_AREA(math)            /* 3.1 maths helpers */
NATIVE_AREA(format)          /* 3.2 number formatting */
NATIVE_AREA(buffer)          /* 3.3 back buffer primitives */
NATIVE_AREA(line)            /* 3.4 line drawing */
NATIVE_AREA(horizon)         /* 3.5 horizon */
NATIVE_AREA(transform)       /* 3.6 3D transform and clipping */
NATIVE_AREA(scenery)         /* 3.8 scenery interpreter */
NATIVE_AREA(text)            /* 3.9 text and font */
NATIVE_AREA(indicator)       /* 3.10 indicators and needles */
NATIVE_AREA(gauges)          /* 3.11 gauge updaters */
NATIVE_AREA(radio)           /* 3.12 radio navigation */
NATIVE_AREA(flight_forces)   /* 3.13 flight model: forces */
NATIVE_AREA(flight_motion)   /* 3.14 flight model: integration and environment */
NATIVE_AREA(editor)          /* 3.16 editor, presets, menus */
NATIVE_AREA(war)             /* 3.17 war mode */
NATIVE_AREA(keys)            /* 3.18 keyboard and controls */
NATIVE_AREA(sound)           /* 3.19 sound */
NATIVE_AREA(sceneryload)     /* 3.20 scenery loading */
NATIVE_AREA(mainloop)        /* 3.21 main loop, frame leftovers, polling loops */
NATIVE_AREA(startup)         /* 3.22 start-up, editor loop, the last original routines */
