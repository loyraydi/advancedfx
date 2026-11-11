# MeowHUD HLAE

Официальный HLAE + команда `mirv_meow_world` (логотипы на стенах и полу CS2 с учётом глубины).

- Код — ветка `meow-world` (разница с тегом из `MEOWHUD_BASE`).
- `.github/workflows/meowhud-hlae.yml` — берёт официальный исходник нужного тега, накладывает патч, собирает `AfxHookSource2.dll` и кладёт её поверх официального архива HLAE → релиз `hlae-meowhud-<версия>.zip`.
- `.github/workflows/meowhud-sync.yml` — каждые 6 часов проверяет новую версию HLAE и новый код в `meow-world`, при необходимости запускает сборку.
- Сборки upstream (`ci.yml`, `archive.yml`) в этом форке выключены.

Лицензия — MIT, как у advancedfx.
