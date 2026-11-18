# Images and local diagrams

Paste an image from the clipboard to create a local attachment. Image files
stay beside the Markdown document; context menus can copy the original image.

![Local attachment with sample data](attachment-example.png)

## Optional Mermaid preview

```mermaid
flowchart LR
    Draft[Markdown draft] --> Edit[Native Edit view]
    Edit --> Read[Read preview]
    Read --> Share[Print or PDF]
```

Mermaid uses an optional external CLI. Without it, the source stays visible.
Write does not embed a browser or send documents to a rendering service.
