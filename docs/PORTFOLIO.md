# Portfolio record

The completed ESP32 controller is registered as Hue Light Control in Better Workspace and linked to [Notion](https://app.notion.com/p/3f5aec944d3c811699d5c9752bb4af87). Its public route is [Hue Light Control](https://zacharysturman.com/projects/hue-light-control).

## Content and storage

- Lifecycle: Complete / Release / Finite; domain: Technology.
- Types: Device, Firmware, Electronics; medium: hardware.
- One liner: A physical Philips Hue controller built with an ESP32, rotary encoder, and OLED display.
- Sources remain local; the project has no Git repository. The Notion project uses `no repo needed`.
- Notion owns portfolio metadata, asset role relations and public work logs. Better Workspace retains this folder's existing identity.
- Artwork originals and PNG exports live in `docs/images/`. Assets use Project storage and paths relative to this folder, with uploaded Notion previews.

## Artwork

All artwork is authored technical illustration, not photography. It depicts the components and documented electrical connections without claiming a manufactured enclosure or fabricated PCB.

| Role | PNG dimensions |
| --- | --- |
| Thumbnail | 1600 × 900 |
| Hero | 2400 × 1350 |
| Banner | 2400 × 600 |
| Poster | 1200 × 1600 |
| Icon | 512 × 512 |
| Wiring / architecture | 1600 × 1230 |

Editable SVGs are regenerated with `python3 scripts/generate_portfolio_artwork.py`. Rasterize each SVG to PNG with an SVG renderer, preserving its declared dimensions. The initial PNG exports use the portfolio's existing Sharp dependency. The portfolio optimizer creates delivery-size variants. The published release serves those through Firebase; R2 upload was skipped because its credentials are unconfigured. Durable deployed URLs, Notion asset IDs and project-relative originals are recorded in `portfolio-assets.json`.

## Work-log provenance

The October 9, 2026 session was recorded under the earlier folder name LonelyBinaryScreen. Its date and original service identity are retained, with curated public wording. The October 10 entry records portfolio packaging and release; it must describe only steps actually completed. Public entries omit private activity metrics, file paths and credentials.

## Validation

Published commit: `b70af8653ed38e28835aec4080090fbc5bed4d52`. [Firebase deployment](https://github.com/ZSturman/ZSDynamics-V5.0/actions/runs/38044685449) succeeded; both production domains expose the exact commit marker. The live project page and API entry passed desktop/mobile checks with analytics suppressed: homepage card, Complete status, hardware metadata, description, primary media roles, wiring gallery, two work logs, lightbox and no horizontal overflow or page errors.

Firmware compilation passed. The portfolio's affected Python files passed 141 tests and 10 subtests; media smoke checks passed 33 tests with 5 suite-defined skips. Both the hosted pipeline and clean release static build passed, and portfolio validation passed for 40 projects and 9 articles. Firmware operation remains builder-reported completion. The Notion page icon and cover use durable Firebase image URLs, verified with HTTP 200.

## Limitations

- One Hue room or zone; a Hue bridge and 2.4 GHz Wi-Fi are required.
- Credentials are configured at build time; no web provisioning interface or source download is published.
- Bridge HTTPS uses the firmware's existing certificate-validation behavior; avoid treating it as authenticated certificate pinning.
- Real device photographs and a filmed demo can replace or supplement the illustrations later.
