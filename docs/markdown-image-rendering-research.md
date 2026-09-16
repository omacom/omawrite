# Pesquisa: imagens em Markdown

Data: 2026-09-15

## Escopo e conclusão

Para o Omawrite, a menor implementação segura é uma **pré-visualização somente
leitura** que use o suporte Markdown já fornecido pelo Qt, mantendo o editor
atual como texto puro. Não se deve substituir o documento editável por um
`QTextDocument` com imagens incorporadas: objetos incorporados são representados
por `U+FFFC` no texto simples, o que quebra o requisito de preservar literalmente
o Markdown salvo e os índices atuais de busca.

No preview, suportar inicialmente somente URLs relativas ao arquivo Markdown.
Elas devem ser resolvidas contra o diretório do documento aberto e precisam
permanecer dentro dele (ou, se houver uma raiz de workspace no futuro, dentro
dessa raiz). Recusar URLs com esquema, incluindo `http`, `https`, `file`, `data`
e `qrc`; isso evita rede, leitura arbitrária de arquivos e payloads embutidos.
Não há necessidade de um parser, fetcher HTTP ou biblioteca nova.

## Sintaxe a aceitar

CommonMark define imagens como links precedidos de `!`:

```markdown
![texto alternativo](images/diagram.png "título opcional")
```

Também há formas por referência. A descrição transforma-se no texto alternativo;
na renderização HTML padrão ela vira `alt`, e apenas o conteúdo textual simples
deve ser usado. Para a primeira entrega, basta o formato inline acima, desde que
a opção seja documentada; referências podem esperar até que haja demanda real.

## Dados do repositório que importam

- O editor é um `TextEdit` com `QSyntaxHighlighter`, e hoje armazena o Markdown
  como texto cru.
- Qt já expõe `TextEdit.MarkdownText`, cujo dialeto é CommonMark com extensões
  GitHub para tabelas e listas de tarefa. Ele é apropriado para uma superfície
  distinta de preview, não para edição WYSIWYG: a própria documentação diz que
  digitar Markdown interativamente nesse modo não é suportado.
- `QTextDocument::baseUrl` resolve URLs relativas a partir do documento. A
  política de confinamento deve ser aplicada *depois* dessa resolução e da
  normalização de caminho, para bloquear `../` que saia da pasta permitida.

## Segurança e limites

1. Não converter Markdown em HTML manualmente, nem aceitar HTML cru para
   obter imagens. Se for usado `QTextDocument::setMarkdown` em C++, passar
   `QTextDocument::MarkdownNoHTML` junto ao dialeto escolhido: o Qt descarta
   tags HTML com essa opção.
2. O carregador deve permitir apenas arquivos regulares em um diretório
   permitido; ao falhar, mostrar o Markdown ou um placeholder, sem tentar uma
   URL alternativa.
3. Para uma implementação que carregue bytes diretamente, usar `QImageReader`;
   ele permite reduzir a imagem com `setScaledSize` e possui um limite global
   de alocação. O Qt rejeita imagens que exigiriam memória acima desse limite.
   O preview deve impor também uma dimensão máxima de exibição.
4. Não habilitar downloads remotos implicitamente. Caso sejam pedidos depois,
   eles precisam de consentimento explícito, timeout, limite de bytes e cache;
   isso é outro recurso, não uma extensão da imagem local.

## Verificação mínima ao implementar

Criar um teste/manual check com: imagem relativa válida; `../fora.png` recusada;
`https://example.test/a.png` recusada; imagem inválida/grande sem crash; e texto
fonte ainda acessível ao retornar do preview quando uma imagem não carregar.
Testar que salvar depois de alternar o preview conserva exatamente
`![alt](images/a.png)`.

## Arquitetura mínima do modo preview (Qt Quick)

### Decisão

Não alternar o `TextEdit` atual entre `PlainText` e `MarkdownText`. O editor é a
fonte única do Markdown, tem `QSyntaxHighlighter`, preserva offsets para busca e
é a fonte de `currentDocumentText()` para salvar, recuperação e contagem. O Qt
também não suporta digitar markup interativamente em modo WYSIWYG.

Adicionar uma segunda superfície, somente leitura, no mesmo `Flickable`:

```
editor TextEdit (PlainText) ── texto ──> Backend::setPreviewMarkdown()
                                              │
                                              ▼
                               QTextDocument de preview (Markdown + imagens)
                                              │
                                              ▼
                                 preview TextEdit (readOnly: true)
```

O toggle é uma propriedade QML `previewVisible`; um botão no rodapé e um único
`Shortcut` a invertem. Quando falso, o editor é visível e tem foco; quando
verdadeiro, o preview é visível e o editor fica oculto. Não criar uma janela,
modelo, arquivo temporário ou estado de documento adicional.

### APIs concretas

1. Em `Backend`, manter um `QTextDocument` exclusivo do preview e expor apenas
   `attachPreviewDocument(QObject *)` e `setPreviewMarkdown(const QString &)`,
   ambos `Q_INVOKABLE`. Ao anexar, converter o `TextDocument` QML em
   `QQuickTextDocument` e chamar `setTextDocument(previewDocument)`. Essa API
   existe desde Qt 6.7; portanto, antes de adotá-la, confirmar que a versão
   mínima distribuída pelo pacote é 6.7. Caso não seja, a alternativa curta é
   manter o preview no QML com `TextEdit { readOnly: true; textFormat:
   TextEdit.MarkdownText }`, mas ela não atende à política de URLs abaixo.
2. `setPreviewMarkdown()` chama `previewDocument->setMarkdown(markdown,
   QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML)`.
   `MarkdownNoHTML` bloqueia HTML cru; o dialeto GitHub coincide com o que
   `TextEdit.MarkdownText` declara oferecer.
3. Ao abrir/salvar como outro arquivo, atualizar `previewDocument->setBaseUrl`
   para a URL local do Markdown. `QTextDocument` resolve `images/a.png` contra
   essa base. Para um documento não salvo, não há base e imagens relativas não
   carregam — comportamento desejável, sem adivinhar uma pasta.
4. O `TextEdit` de preview recebe `readOnly: true`, `selectByMouse: true`, a
   mesma largura e tipografia base do editor, e seu `implicitHeight` alimenta o
   `contentHeight` do `Flickable`. Ele não chama `attachDocument()`,
   `editorTextChanged()` nem recebe o `MarkdownHighlighter`.

### Carregamento seguro de imagem

`Text { textFormat: Text.MarkdownText }` e qualquer `Text` não-plain podem
carregar imagens remotas; a documentação do Qt alerta explicitamente sobre
isso. Portanto, a variante QML-only não deve ser usada para Markdown que possa
vir de arquivos externos.

Para manter a política local proposta acima, o `QTextDocument` do preview deve
ser uma subclasse pequena que sobrescreve `loadResource(int, const QUrl &)`: para
`ImageResource`, aceitar somente uma URL local, normalizar/canonicalizar o
caminho e confirmar que está sob a pasta do Markdown (ou futura raiz de
workspace); todo outro esquema, caminho fora da raiz, arquivo não regular ou
falha de decode retorna um `QVariant` vazio. Usar `QImageReader` nessa rotina
para checar/decode, aplicar `setScaledSize()` quando apropriado e respeitar seu
limite de alocação. Para os demais tipos, delegar ao `QTextDocument`.

Não basta `setResourceProvider()`: a documentação do Qt determina que
`loadResource()` é tentado antes do provider. O override é o ponto único que
impede que o caminho padrão alcance a rede ou arquivos não permitidos.

### Fluxo de sincronização

1. `Component.onCompleted`: anexar ambos os documentos. O documento principal
   continua exatamente como está.
2. `editor.onTextChanged`: executar o fluxo existente (`editorTextChanged`,
   busca, modified, recovery) e, se `previewVisible`, reiniciar um `Timer`
   QML de 100–150 ms que chama `backend.setPreviewMarkdown(editor.text)`.
   Assim uma digitação rápida não reanalisa todo o documento a cada tecla.
3. Ao ligar o preview, parar o timer pendente, chamar imediatamente
   `setPreviewMarkdown(editor.text)`, mostrar o preview e opcionalmente levar o
   `Flickable.contentY` ao início. Ao desligar, parar o timer e devolver foco ao
   editor. O preview nunca escreve em `editor.text`.
4. Depois de `open()`, `reloadFromDisk()` ou uma restauração de recovery, o
   `onTextChanged` já entrega o novo texto; se o preview estiver aberto, ele é
   renderizado. Após `setFileUrl()`, atualizar também a base URL e re-renderizar
   se o preview estiver aberto. Salvar sem trocar URL não exige nova conversão.

### Limites deliberados da primeira entrega

- Apenas `![alt](caminho-relativo)`; não adicionar arrastar-e-soltar, upload,
  gerenciador de anexos, dimensões Markdown, imagens remotas ou cache.
- Falha de imagem não abre diálogo; a fonte continua disponível ao voltar do
  preview. Texto alternativo visível exigiria um renderer próprio e fica fora
  desta primeira entrega.
- Um preview inteiro, não preview lado a lado nem sincronização de posição entre
  fonte e renderização.
- Imagens grandes: limitar pixels/memória no carregador e exibir no máximo a
  largura da coluna. Animação e SVG devem permanecer fora do escopo até serem
  explicitamente definidos e testados.

## Fontes primárias

- [CommonMark 0.31.2 — Images](https://spec.commonmark.org/0.31.2/#images):
  gramática, exemplos e regra de texto alternativo.
- [Qt 6 — TextEdit](https://doc.qt.io/qt-6/qml-qtquick-textedit.html):
  `MarkdownText`, acesso ao `QTextDocument` e limitação de edição WYSIWYG.
- [Qt 6 — QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html):
  `baseUrl`, recursos de imagem, `MarkdownNoHTML`, `setMarkdown` e representação
  de objetos incorporados.
- [Qt 6 — QImageReader](https://doc.qt.io/qt-6/qimagereader.html):
  escala no carregamento e `setAllocationLimit` para limitar alocação.
- [Qt 6 — QQuickTextDocument](https://doc.qt.io/qt-6/qquicktextdocument.html):
  acesso ao documento de um `TextEdit` e `setTextDocument()` desde Qt 6.7.
- [Qt 6 — Text](https://doc.qt.io/qt-6/qml-qtquick-text.html): aviso de que
  formatos ricos, inclusive Markdown, podem carregar imagens remotas.
