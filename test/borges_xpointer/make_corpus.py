#!/usr/bin/env python3
"""Builds the differential corpus: hand-made hostile EPUBs, plus the spine of every EPUB found.

usage: make_corpus.py OUT_DIR [EPUB_OR_DIR ...]

For each book, OUT_DIR/<name>/ gets book.epub and spine/NNNN.xhtml, in the order the firmware's
ContentOpfParser builds its spine: every itemref whose idref is in the manifest. The hostile
books are also written to test/borges_xpointer/fixtures/ so the regressions can name them.
"""
import pathlib
import re
import shutil
import sys
import xml.etree.ElementTree as ET
import zipfile

XHTML = '<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">\n<head><title>t</title><link rel="stylesheet" type="text/css" href="s.css"/></head>\n<body>%s</body>\n</html>\n'
XHTML_DTD = '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">\n<html xmlns="http://www.w3.org/1999/xhtml">\n<head><title>t</title></head>\n<body>%s</body>\n</html>\n'

PAD = "padding padding padding padding padding padding padding padding padding padding"

# name -> body. Each becomes its own spine item, so a mismatch names its case.
CASES = {
    # The defects the review found.
    "nested_p": "<p>one</p><blockquote><p>q1</p><p>q2</p></blockquote><p>two</p><p>three</p>",
    "nested_div": "<div><div><p>inner</p></div><p>in-first</p></div><div><p>second-div</p></div>",
    "anchor_first": "<p>" + PAD + "</p><p><a id=\"pg12\"/>Hello world again</p><p><a id=\"x\"></a>Second anchored</p>",
    "inline_runs": "<p><i>Foo</i> bar one <b>baz</b> qux two</p><p>Lead <span>inner text</span> tail text</p>",
    "div_first_ws": "<div>\n<p>bbb</p></div><div>\n  <p>ccc</p>\n  <p>ddd</p>\n</div>",
    "page_at_image": "<div><p>aaa</p>\n<img src=\"x.png\" alt=\"i\"/>\n<p>bbb</p></div>",
    "wrapped": "<p>\n        Hello\n        world\n        again</p>\n<p>  two   spaces\there  </p>",
    "div_lead_tail": "<div>Lead <span>inner</span> tail text</div>",
    # Whitespace nodes and how they survive.
    "mixed_block_inline": "<div>Lead <p>x</p> <p>y</p> tail <p>z</p>\n after <b>b</b> <i>i</i> end\n</div>",
    "ws_between_inlines": "<p><b>a</b> <i>b</i>\n<u>c</u> d</p><div><span>a</span> <span>b</span> c</div>",
    "ws_edges": "<div> <span>a</span> </div><span> <b>b</b> c </span><p> </p><p>\n</p><p> x </p>",
    "inline_holding_block": "<span> <p>a</p> <p>b</p> tail <b>c</b></span><p>f</p>",
    "body_text": "Direct text <b>in</b> body\n<p>then</p>\nmore <i>direct</i> text\n",
    "comments": "<p>a<!-- c -->b<!-- d --> <!-- e -->c</p><div><!-- f --> <!-- g --><p>d</p><!-- h --> e</div>",
    "cdata": "<p>a<![CDATA[b & <c>]]>d</p><p><![CDATA[  e  f  ]]></p><p>g<![CDATA[]]>h</p>",
    "pi": "<p>a<?foo bar?>b</p><div><?baz?> <p>c</p></div>",
    "br_img": "<p>a<br/>b<br/> c <br/>\nd</p><p><img src=\"x.png\" alt=\"i\"/> e <img src=\"x.png\" alt=\"i\"/></p>",
    "empty_elements": "<p><a id=\"a\"/></p><p><span></span>x<span/>y</p><p/>z<hr/>w<div/>v",
    "nested_inline": "<p><b><i><u>deep</u> mid</i> shallow</b> top <b>again <i>and</i> more</b> end</p>",
    # Characters.
    "entities": "<p>a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos; &#65;&#x42; f</p><p>&#32;&#32;g &#32; h&#10;i&#9;j</p><p>&#150;k&#8212;l&#x1F600;m</p>",
    "unicode": "<p>año ñandú — “quotes” 日本語テキスト 😀 x 😀😀 y</p><p>é combining א עברית مرحبا</p>",
    "nbsp_shy": "<p>a b  c     d soft­hyphen­ word</p><p> </p><p>   </p><div> <p>e</p> </div>",
    "bom_inner": "<p>﻿a﻿ b</p><p>​zero​ width em thin</p>",
    # Structure.
    "tables": "<table>\n<caption> cap </caption>\n<thead><tr><th>h1</th>\n<th> h2 </th></tr></thead>\n<tbody>\n<tr>\n<td>c1</td>\n<td>c2 <b>x</b> y</td>\n</tr>\n<tr><td><p>p1</p> <p>p2</p></td><td>a<table><tr><td>nested</td></tr></table>b</td></tr>\n</tbody>\n</table>after",
    "incomplete_table": "<div><td>lone cell</td> <td>two</td></div><div><tr><td>row</td></tr></div>",
    "lists": "<ul>\n<li>one</li>\n<li>two <b>b</b> t\n<ul>\n<li>in</li> <li>ner</li>\n</ul>\ntail</li>\n<li><p>p</p></li>\n</ul><ol><li>x</li></ol><dl><dt>t</dt> <dd>d</dd></dl>",
    "sections": "<section><h1>h</h1>\n<section><h2>s</h2><p>a</p></section>\n<section><p>b</p></section></section><article>c<aside>d</aside>e</article><figure><img src=\"x.png\" alt=\"i\"/><figcaption>f</figcaption></figure>",
    "unknown_tags": "<foo>a</foo> <bar>b <baz>c</baz></bar><p><foo>d</foo> <foo>e</foo> f</p>",
    "fb2ish_tags": "<epigraph><p>a</p></epigraph><poem>b<stanza><v>c</v> <v>d</v></stanza></poem><subtitle>e</subtitle><cite>f</cite><date>g</date>",
    "uppercase": "<DIV>A<P>B</P> <P>C</P></DIV><p><SPAN>d</SPAN> <span>e</span> f</p>",
    "namespaced": "<p><epub:switch><epub:case>a</epub:case><epub:default>b</epub:default></epub:switch> c</p><section epub:type=\"chapter\"><p>d</p></section>",
    "ruby": "<p>a<ruby>漢<rt>kan</rt>字<rp>(</rp><rt>ji</rt><rp>)</rp></ruby>b</p><p><ruby> <rb>c</rb> <rt>d</rt> </ruby> e</p>",
    "svg_inline": "<p>a<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><title>tt</title><text> st </text><g>loose</g></svg>b</p><p>c</p>",
    "object_text": "<p>a<object data=\"x\">fallback <b>bold</b></object>b</p><audio>af</audio><video>vf</video>d",
    "style_in_body": "<p>a</p><style>p { color: red }</style><p>b<script>var x = 1;</script>c</p><title>tt</title><p>d</p>",
    "pre": "<pre>\nfirst newline\n  keep  spaces\n</pre><pre>x\ty\tz\n\tw</pre><pre><b>m</b>\n\nn</pre><div><pre> </pre>o</div>",
    "white_space_inline": "<p style=\"white-space: pre\">a  b\n  c</p><p style=\"white-space:pre-wrap\"><b>d</b>  e</p><div style=\"white-space: pre-line\">f  g\nh</div><p style=\"white-space: normal\">i  j</p>",
    "pre_child_normal": "<pre>a  b<span style=\"white-space:normal\">c   d</span>e  f</pre>",
    "display_inline_style": "<div><span style=\"display:block\">a</span> <span style=\"display:block\">b</span> tail</div><div><p style=\"display:inline\">c</p> <p style=\"display:inline\">d</p> e</div><div>\n<b style=\"display:none\">f</b> g</div>",
    "br_display_block": "<h1 class=\"blk\">\nLead words <br class=\"blk\"/>\n<br class=\"blk\"/>\nSecond part <br class=\"blk\"/>\n</h1><p>a<br style=\"display:block\"/> <br style=\"display:block\"/> b</p><p>c<br class=\"gone\"/> <br class=\"gone\"/> d</p>",
    "display_css": "<div><span class=\"blk\">a</span> <span class=\"blk\">b</span> tail</div><div><p class=\"inl\">c</p> <p class=\"inl\">d</p> e</div><div><p class=\"gone\">f</p> <p>g</p> h</div><div class=\"gone\">\n<p>i</p>\n<p>j</p></div>",
    "white_space_css": "<p class=\"keep\">a  b\n  c</p><p>d  e</p>",
    "float_inline_block": "<div><div style=\"float:left\">a</div> <p>b</p> c</div><div><span style=\"display:inline-block\">d</span> <p>e</p> f</div>",
    "text_transform": "<p style=\"text-transform:uppercase\">abc def</p><p class=\"up\">ghi</p>",
    "first_letter_line": "<p class=\"fl\">Drop cap text here</p><p class=\"fln\">First line text here that goes on</p><p class=\"bef\">with generated content</p>",
    "long_paragraph": "<p>" + ("word " * 2000) + "</p><p>" + ("x" * 9000) + " after</p><p>" + ("amp&amp;amp " * 1000) + "</p>",
    "long_paragraph_nl": "<p>" + ("word\n" * 2000) + "</p><pre>" + ("line\n" * 2000) + "</pre>",
    "clip_shapes": "<ul><li>first item</li><li>second item</li></ul><ol><li>one</li><li>two</li></ol><p>soft\u00adhy\u00adphen wo<i>rd</i> e\u0301 a\u00a0b &#150; c<span epub:type=\"pagebreak\" title=\"3\">3</span>d</p><p>the cat and the dog and the bird</p>",
    "after_body_hidden": "<p hidden=\"hidden\">a</p><p>b</p><span epub:type=\"pagebreak\" title=\"3\">3</span><p>c</p>",
    "only_image": "<img src=\"x.png\" alt=\"i\"/>",
    "empty_body": "",
}

CSS = (
    ".blk { display: block } .inl { display: inline } .gone { display: none } .keep { white-space: pre }\n"
    ".up { text-transform: uppercase } .fl::first-letter { font-size: 2em } .fln::first-line { font-weight: bold }\n"
    ".bef::before { content: 'generated '; }\n"
)

# Whole documents, for what happens outside <body> or before the first byte.
RAW_DOCS = {
    "dtd_nbsp": (XHTML_DTD % "<p>a&nbsp;b&eacute;c&mdash;d&hellip; e</p><p>&Alpha;&hearts;f</p>").encode(),
    "bom_start": b"\xef\xbb\xbf" + (XHTML % "<p>after bom</p>").encode(),
    "crlf": (XHTML % "<p>a\r\nb  \r\n  c</p>\r\n<div>\r\n<p>d</p>\r\n</div>\r\n<pre>e\r\nf\r\n</pre>").replace("\n", "\r\n").replace("\r\r", "\r").encode(),
    "latin1": ('<?xml version="1.0" encoding="iso-8859-1"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head><body><p>a\xf1o  ni\xf1o</p><p>caf\xe9</p></body></html>').encode("latin-1"),
    "outside_body_text": b'<?xml version="1.0"?>\n<html xmlns="http://www.w3.org/1999/xhtml">\n  <head>\n    <title>t</title>\n  </head>\n  stray\n  <body>\n  <p>a</p>\n  </body>\n  trailing\n</html>\n',
    "body_attrs_pre": b'<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head><body style="white-space:pre">  a  b\n</body></html>',
    "html5_doctype": b'<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head><body>\n<p>a</p>\n</body></html>',
}

PUBLISHER_STYLE = {
    # What Sigil, Calibre and InDesign exports look like, at a chapter's length.
    "calibre": '<div class="calibre"><h2 class="calibre3" id="c1"><a id="p1"></a>Chapter <span class="calibre4">One</span></h2>\n<p class="calibre5">' + PAD + ' <em class="calibre6">emph</em> rest of the text.</p>\n<p class="calibre5"><a id="p2"></a>' + PAD + '<sup class="calibre7"><a href="#n1">1</a></sup> and more.</p>\n<blockquote class="calibre8"><p class="calibre5">quoted</p>\n<p class="calibre5">quoted two</p></blockquote>\n<p class="calibre5">after the quote</p>\n</div>\n',
    "indesign": '<div class="story" id="_idContainer001">\n\t<p class="body-first"><span class="CharOverride-1">T</span>he first <span class="italic">line</span> of it.</p>\n\t<p class="body"><span class="pagebreak" id="p3"/>Then the second.</p>\n\t<div class="_idGenObjectLayout-1"><div class="_idGenObjectStyle-1"><img class="img" src="x.png" alt="i"/></div></div>\n\t<p class="caption">Figure <b>1</b>.</p>\n</div>\n',
    "sigil": '\n\n  <h1 id="sigil_toc_id_1">Title</h1>\n\n  <p>Para one.</p>\n\n  <p>Para <i>two</i>.</p>\n\n  <p><br/></p>\n\n  <p class="center">* * *</p>\n\n  <p>Para three.</p>\n',
}


def opf(items, spine):
    manifest = "".join(f'<item id="{i}" href="{h}" media-type="{m}"/>' for i, h, m in items)
    refs = "".join(s if s.startswith("<") else f'<itemref idref="{s}"/>' for s in spine)
    return (
        '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="b">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Corpus</dc:title><dc:identifier id="b">x</dc:identifier><dc:language>en</dc:language></metadata>'
        f"<manifest>{manifest}</manifest><spine>{refs}</spine></package>"
    )


CONTAINER = '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'


def write_epub(path, docs, extra_items=(), spine_override=None, extra_files=()):
    items = [(f"c{i}", f"{name}.xhtml", "application/xhtml+xml") for i, (name, _) in enumerate(docs)]
    items.append(("css", "s.css", "text/css"))
    items.extend(extra_items)
    spine = spine_override or [f"c{i}" for i in range(len(docs))]
    # A fixed date, so regenerating a fixture does not change its bytes.
    def member(z, name, data, compress=zipfile.ZIP_DEFLATED):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = compress
        z.writestr(info, data)

    with zipfile.ZipFile(path, "w") as z:
        member(z, "mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        member(z, "META-INF/container.xml", CONTAINER)
        member(z, "OEBPS/content.opf", opf(items, spine))
        member(z, "OEBPS/s.css", CSS)
        for name, data in docs:
            member(z, f"OEBPS/{name}.xhtml", data)
        for name, data in extra_files:
            member(z, f"OEBPS/{name}", data)


def build_hostile(fixtures):
    fixtures.mkdir(parents=True, exist_ok=True)
    write_epub(fixtures / "hostile_cases.epub", [(k, XHTML % v) for k, v in CASES.items()])
    write_epub(fixtures / "hostile_documents.epub", list(RAW_DOCS.items()))
    write_epub(fixtures / "publisher_style.epub", [(k, XHTML % v) for k, v in PUBLISHER_STYLE.items()])
    # A spine with everything that could shift DocFragment numbers.
    docs = [("a", XHTML % "<p>first</p>"), ("b", XHTML % "<p>second, non linear</p>"), ("c", XHTML % "<p>third</p>"), ("d", XHTML % "<p>after the rest</p>")]
    write_epub(
        fixtures / "spine_shapes.epub",
        docs,
        extra_items=[("gone", "gone.xhtml", "application/xhtml+xml"), ("im", "i.svg", "image/svg+xml"), ("txt", "n.txt", "text/plain")],
        spine_override=["c0", '<itemref idref="c1" linear="no"/>', "gone", "c2", "im", "txt", '<itemref idref="nosuchid"/>', "c3"],
        extra_files=[("i.svg", '<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><text>svg spine</text></svg>'), ("n.txt", "plain text item")],
    )


def build_known_limits(fixtures):
    # What the firmware does not model, kept apart so it does not hide anything else.
    write_epub(fixtures / "limit_entities.epub", [("e", (XHTML_DTD % "<p>a &unknownname; b &NotEqualTilde; c &CounterClockwiseContourIntegral; d</p><p>after</p>").encode())])
    write_epub(fixtures / "limit_opaque.epub", [
        ("table_stray_text", XHTML % "<table>stray<tr>loose<td>cell</td>more</tr>tail</table>"),
        ("math", XHTML % "<p>a<math xmlns=\"http://www.w3.org/1998/Math/MathML\"><mi> x </mi><mo>=</mo><mn>1</mn></math>b</p><p>c</p>"),
        ("empty_holder", XHTML % "<span> <p>d</p> <p>e</p> </span><p>f</p>"),
    ])
    write_epub(fixtures / "limit_depth.epub", [("d", XHTML % ("<div>" * 60 + "deep text" + "</div>" * 60 + "<p>after</p>"))])
    write_epub(fixtures / "limit_nested_body.epub", [("n", XHTML % "<p>a</p><body><p>b</p></body><p>c</p>")])
    write_epub(fixtures / "limit_two_bodies.epub", [("a", (XHTML % "<p>first</p>").replace("</html>", "<body><p>second</p></body></html>").encode()), ("b", (XHTML % "<p>next file</p>").encode())])


def local(tag):
    return tag.rsplit("}", 1)[-1]


def extract(epub, out):
    with zipfile.ZipFile(epub) as z:
        container = ET.fromstring(z.read("META-INF/container.xml"))
        rootfile = next(e for e in container.iter() if local(e.tag) == "rootfile").get("full-path")
        base = rootfile.rsplit("/", 1)[0] + "/" if "/" in rootfile else ""
        package = ET.fromstring(z.read(rootfile))
        manifest = {e.get("id"): e.get("href") for e in package.iter() if local(e.tag) == "item"}
        sheets = [e.get("href") for e in package.iter() if local(e.tag) == "item" and e.get("media-type") == "text/css"]
        spine = [e.get("idref") for e in package.iter() if local(e.tag) == "itemref"]
        names = set(z.namelist())
        target = out / re.sub(r"[^A-Za-z0-9_.-]", "_", epub.stem)
        if target.exists():
            shutil.rmtree(target)
        (target / "spine").mkdir(parents=True)
        (target / "css").mkdir()
        for i, href in enumerate(sheets):
            if base + href in names:
                (target / "css" / f"{i:04d}.css").write_bytes(z.read(base + href))
        shutil.copy(epub, target / "book.epub")
        count = 0
        for idref in spine:
            if idref not in manifest:
                continue
            name = base + manifest[idref]
            data = z.read(name) if name in names else b""
            (target / "spine" / f"{count:04d}.xhtml").write_bytes(data)
            count += 1
    return target, count


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    build_hostile(fixtures)
    build_known_limits(fixtures)
    sources = sorted(fixtures.glob("*.epub"))
    for arg in sys.argv[2:]:
        path = pathlib.Path(arg)
        sources.extend(sorted(path.glob("**/*.epub")) if path.is_dir() else [path])
    for epub in sources:
        target, count = extract(epub, out)
        print(f"{target.name}\t{count}")


if __name__ == "__main__":
    main()
