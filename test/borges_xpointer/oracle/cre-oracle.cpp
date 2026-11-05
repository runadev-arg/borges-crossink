// cre-oracle: loads an EPUB through KOReader's crengine exactly as KOReader does and answers
// questions about its DOM and xpointers. It is the reference the firmware mapper is held to.
//
//   cre-oracle <book.epub> [--dom N] [--flags HEX] [--w W] [--h H] [--css FILE] [--font FILE]
//              [--enum OUT] [--resolve IN --out OUT]
//
// --enum writes one line per body text node and per page start:
//   N <frag> <xpointer-at-offset-0> <length> <escaped text>
//   P <page0> <xpointer> <frag> <ordinal> <percent*100>
// --resolve reads one xpointer per line and writes, for each:
//   R <input> <valid> <normalized> <kind> <offset> <charHex> <frag> <ordinal> <page0> <percent*100>
// `ordinal` is the count of non-space characters in the body text nodes of that DocFragment
// before the position: a coordinate that does not depend on either side's node numbering.
#include <crengine.h>
#include <fb2def.h>
#include <lvdocview.h>
#include <lvrend.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {
std::string utf8(const lString32& s) {
  const lString8 u = UnicodeToUtf8(s);
  return std::string(u.c_str(), u.length());
}

std::string escape(const lString32& s) {
  const std::string in = utf8(s);
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
    }
  }
  return out;
}

bool isSpaceChar32(const lChar32 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

struct NodeInfo {
  int frag = 0;
  int ordinalBefore = 0;
};

struct Index {
  std::map<ldomNode*, NodeInfo> nodes;
  std::vector<ldomNode*> order;
};

// Internal copies, not positions: ::first-line clones and generated content.
bool isInternal(ldomNode* n) {
  const lUInt16 id = n->getNodeId();
  return id == el_cloneNode || id == el_pseudoElem;
}

void buildIndex(ldomDocument* doc, Index& index) {
  struct Frame {
    ldomNode* node;
    int next;
  };
  ldomNode* root = doc->getRootNode();
  int frag = 0;
  int ordinal = 0;
  std::vector<Frame> stack;
  stack.push_back({root, 0});
  std::vector<bool> inBody;
  inBody.push_back(false);
  while (!stack.empty()) {
    Frame& top = stack.back();
    if (top.next >= static_cast<int>(top.node->getChildCount())) {
      stack.pop_back();
      inBody.pop_back();
      continue;
    }
    ldomNode* child = top.node->getChildNode(top.next++);
    const bool bodyHere = inBody.back();
    if (child->isText()) {
      if (bodyHere) {
        index.nodes[child] = NodeInfo{frag, ordinal};
        index.order.push_back(child);
        const lString32 text = child->getText();
        for (int i = 0; i < text.length(); i++) {
          if (!isSpaceChar32(text[i])) ordinal++;
        }
      }
      continue;
    }
    if (isInternal(child)) continue;
    bool childInBody = bodyHere;
    if (child->getNodeId() == el_DocFragment) {
      frag++;
      ordinal = 0;
    } else if (!bodyHere && child->getNodeId() == el_body && top.node->getNodeId() == el_DocFragment) {
      childInBody = true;
    }
    stack.push_back({child, 0});
    inBody.push_back(childInBody);
  }
}

int fragOfNode(ldomNode* n) {
  int frag = 0;
  for (; n; n = n->getParentNode()) {
    if (n->getNodeId() == el_DocFragment) {
      ldomNode* parent = n->getParentNode();
      for (int i = 0; parent && i < static_cast<int>(parent->getChildCount()); i++) {
        ldomNode* c = parent->getChildNode(i);
        if (c->isElement() && c->getNodeId() == el_DocFragment) frag++;
        if (c == n) return frag;
      }
    }
  }
  return 0;
}

// First indexed text node at or after `node` in document order, descending into it first.
ldomNode* firstTextAtOrAfter(ldomNode* node, const int childOffset, const Index& index) {
  ldomNode* n = node;
  int next = childOffset < 0 ? 0 : childOffset;
  while (n) {
    if (n->isText()) {
      if (index.nodes.count(n)) return n;
    } else if (next < static_cast<int>(n->getChildCount())) {
      n = n->getChildNode(next);
      next = 0;
      continue;
    }
    ldomNode* parent = n->getParentNode();
    if (!parent) return nullptr;
    next = n->getNodeIndex() + 1;
    n = parent;
  }
  return nullptr;
}

void locate(ldomXPointer xp, const Index& index, int& frag, int& ordinal) {
  frag = 0;
  ordinal = 0;
  ldomNode* node = xp.getNode();
  if (!node) return;
  frag = fragOfNode(node);
  if (node->isText()) {
    const auto it = index.nodes.find(node);
    if (it == index.nodes.end()) {
      ordinal = -1;
      return;
    }
    ordinal = it->second.ordinalBefore;
    const lString32 text = node->getText();
    const int offset = xp.getOffset() < 0 ? 0 : xp.getOffset();
    for (int i = 0; i < offset && i < text.length(); i++) {
      if (!isSpaceChar32(text[i])) ordinal++;
    }
    return;
  }
  ldomNode* text = firstTextAtOrAfter(node, xp.getOffset(), index);
  if (!text) {
    ordinal = -2;  // nothing follows in the document
    return;
  }
  const auto it = index.nodes.find(text);
  if (it->second.frag != frag) {
    ordinal = -3;  // nothing follows in this fragment
    return;
  }
  ordinal = it->second.ordinalBefore;
}

int percentOf(LVDocView& view, ldomXPointer xp) {
  const lvPoint pt = xp.toPoint(true);
  const int full = view.GetFullHeight();
  if (full <= 0 || pt.y < 0) return -1;
  return static_cast<int>(static_cast<long long>(pt.y) * 10000 / full);
}
}  // namespace

int main(int argc, char** argv) {
  // crengine opens files in the default mode, which on POSIX is always binary.
  _set_fmode(_O_BINARY);
  if (argc < 2) {
    fprintf(stderr, "usage: cre-oracle book.epub [--dom N] [--flags HEX] [--enum OUT] [--resolve IN --out OUT]\n");
    return 2;
  }
  const char* book = argv[1];
  int dom = gDOMVersionCurrent;
  unsigned flags = 0x7FFFFFFF;  // KOReader's default block_rendering_mode 3, "web"
  int width = 600;
  int height = 800;
  std::string cssPath = "D:/CrossInkBorges/crossink-tools/crengine/src/cr3gui/data/epub.css";
  std::string fontPath = "C:/Windows/Fonts/arial.ttf";
  std::string enumPath, resolvePath, outPath;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    const char* v = argv[i + 1];
    if (a == "--dom")
      dom = atoi(v);
    else if (a == "--flags")
      flags = static_cast<unsigned>(strtoul(v, nullptr, 16));
    else if (a == "--w")
      width = atoi(v);
    else if (a == "--h")
      height = atoi(v);
    else if (a == "--css")
      cssPath = v;
    else if (a == "--font")
      fontPath = v;
    else if (a == "--enum")
      enumPath = v;
    else if (a == "--resolve")
      resolvePath = v;
    else if (a == "--out")
      outPath = v;
    else {
      fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }

  // Same bring-up as koreader-base cre.cpp: font manager, hyphenation, then a view with
  // noDefaultDocument, the requested DOM version and the block rendering flags.
  InitFontManager(lString8::empty_str);
  if (!fontMan->RegisterFont(lString8(fontPath.c_str()))) {
    fprintf(stderr, "could not register font %s\n", fontPath.c_str());
    return 3;
  }
  HyphMan::initDictionaries(lString32::empty_str);

  LVDocView view(-1, true);
  view.setViewMode(DVM_PAGES, -1);
  view.Resize(width, height);
  CRPropRef props = view.propsGetCurrent();
  props->setString(PROP_FONT_FACE, "Arial");
  props->setString(PROP_FALLBACK_FONT_FACES, "Arial");
  props->setInt(PROP_REQUESTED_DOM_VERSION, dom);
  props->setInt(PROP_RENDER_BLOCK_RENDERING_FLAGS, static_cast<int>(flags));
  props->setInt(PROP_EMBEDDED_STYLES, 1);
  props->setInt(PROP_EMBEDDED_FONTS, 0);
  view.propsApply(props);

  lString8 css;
  if (!LVLoadStylesheetFile(Utf8ToUnicode(cssPath.c_str()), css)) {
    fprintf(stderr, "could not load stylesheet %s\n", cssPath.c_str());
    return 3;
  }
  view.setStyleSheet(css);

  if (!view.LoadDocument(book)) {
    fprintf(stderr, "could not load %s\n", book);
    return 4;
  }
  view.Render();
  ldomDocument* doc = view.getDocument();
  if (!doc) {
    fprintf(stderr, "no document for %s\n", book);
    return 4;
  }

  Index index;
  buildIndex(doc, index);

  if (!enumPath.empty()) {
    FILE* out = fopen(enumPath.c_str(), "wb");
    if (!out) return 5;
    fprintf(out, "D\t%d\t0x%x\t%d\n", doc->getDOMVersionRequested(), doc->getRenderBlockRenderingFlags(),
            view.getPageCount());
    for (ldomNode* node : index.order) {
      const lString32 text = node->getText();
      ldomXPointer xp(node, 0);
      fprintf(out, "N\t%d\t%s\t%d\t%s\n", index.nodes[node].frag, utf8(xp.toString()).c_str(), text.length(),
              escape(text).c_str());
    }
    const int pages = view.getPageCount();
    for (int p = 0; p < pages; p++) {
      ldomXPointer xp = view.getPageBookmark(p, true, false);
      int frag = 0;
      int ordinal = 0;
      if (!xp.isNull()) locate(xp, index, frag, ordinal);
      fprintf(out, "P\t%d\t%s\t%d\t%d\t%d\n", p, xp.isNull() ? "" : utf8(xp.toString()).c_str(), frag, ordinal,
              xp.isNull() ? -1 : percentOf(view, xp));
    }
    fclose(out);
  }

  if (!resolvePath.empty()) {
    FILE* in = fopen(resolvePath.c_str(), "rb");
    FILE* out = outPath.empty() ? stdout : fopen(outPath.c_str(), "wb");
    if (!in || !out) return 5;
    char line[4096];
    while (fgets(line, sizeof(line), in)) {
      std::string s(line);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      ldomXPointer xp = doc->createXPointer(Utf8ToUnicode(s.c_str()));
      if (xp.isNull()) {
        fprintf(out, "R\t%s\t0\t\t\t\t\t\t\t\t\n", s.c_str());
        continue;
      }
      ldomXPointer copy = xp;
      ldomNode* node = xp.getNode();
      const int offset = xp.getOffset();
      unsigned ch = 0;
      if (node->isText() && offset >= 0) {
        const lString32 text = node->getText();
        if (offset < text.length()) ch = static_cast<unsigned>(text[offset]);
      }
      int frag = 0;
      int ordinal = 0;
      locate(xp, index, frag, ordinal);
      fprintf(out, "R\t%s\t1\t%s\t%s\t%d\t%x\t%d\t%d\t%d\t%d\n", s.c_str(), utf8(copy.toStringV2()).c_str(),
              node->isText() ? "text" : "elem", offset, ch, frag, ordinal, view.getBookmarkPage(xp),
              percentOf(view, xp));
    }
    fclose(in);
    if (out != stdout) fclose(out);
  }
  return 0;
}
