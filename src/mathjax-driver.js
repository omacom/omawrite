// Runs after the MathJax components in a bare QJSEngine: no DOM, no loader
// network access. Typesets through MathJax's lite adaptor straight to SVG.
var omawriteRenderMath = (function () {
    var _ = MathJax._;
    var adaptor = _.adaptors.liteAdaptor.liteAdaptor();
    _.handlers.html_ts.RegisterHTMLHandler(adaptor);

    // Without noerrors and noundefined, a TeX error throws instead of being
    // typeset as red source, so the editor can leave the source in place.
    var packages = _.input.tex.AllPackages.AllPackages.filter(function (name) {
        return name !== "noerrors" && name !== "noundefined";
    });
    var tex = new _.input.tex_ts.TeX({
        packages: packages,
        formatError: function (jax, error) { throw error; }
    });
    // The SVG output's built-in font lacks the large-operator variants; the
    // full TeX font has to be passed in explicitly or \sum and \int fail.
    var svg = new _.output.svg_ts.SVG({
        fontCache: "local",
        font: new _.output.svg.fonts.tex_ts.TeXFont()
    });
    var html = _.mathjax.mathjax.document("", {InputJax: tex, OutputJax: svg});

    function ex(value) {
        return parseFloat(value) || 0;
    }

    return function (source, display) {
        try {
            var node = adaptor.firstChild(html.convert(source, {display: display}));
            return {
                svg: adaptor.outerHTML(node),
                width: ex(adaptor.getAttribute(node, "width")),
                height: ex(adaptor.getAttribute(node, "height")),
                depth: -ex(adaptor.getStyle(node, "vertical-align"))
            };
        } catch (error) {
            // TeX errors are plain objects, not Error instances.
            return {error: String((error && error.message) || error)};
        }
    };
})();
