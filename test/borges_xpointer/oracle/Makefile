# Builds cre-oracle with llvm-mingw: mingw32-make -j8 CRE=<crengine checkout> OUT=<object dir>
CRE ?= D:/CrossInkBorges/crossink-tools/crengine/src
OUT ?= C:/CrossInkBuild/cre
DEPS ?= D:/CrossInkBorges/crossink-tools/crengine/deps
HERE := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
CC := clang
CXX := clang++
CRSRC := chmfmt cp_stats cri18n crtxtenc docxfmt epubfmt fb3fmt hist hyphman lstridmap lvdocview lvdrawbuf lvfnt lvfntman lvimg lvmemman lvopc lvpagesplitter lvrend lvstream lvstring lvstsheet lvstyles lvtextfm lvtinydom lvxml mathml mdfmt odtfmt odxutil pdbfmt props renderutil rtfimp textlang txtselector wordfmt
ZSRC := adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil
FTSRC := lzw/ftlzw base/ftsynth pfr/pfr type42/type42 winfonts/winfnt pcf/pcf bdf/bdf psaux/psaux sdf/sdf svg/svg base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbbox base/ftglyph base/ftbitmap base/ftmm autofit/autofit truetype/truetype sfnt/sfnt smooth/smooth raster/raster psnames/psnames pshinter/pshinter cff/cff type1/type1 cid/type1cid gzip/ftgzip
INC := -I$(HERE)compat -I$(DEPS)/xxHash-0.8.3 -I$(CRE)/crengine/include -I$(DEPS)/freetype-VER-2-14-3/include -I$(DEPS)/zlib-1.3.2 -I$(HERE)
DEFS := -include $(HERE)crsetup_oracle.h -DFT2_BUILD_LIBRARY -DNDEBUG -DXXH_INLINE_ALL
CXXFLAGS := -std=c++17 -O1 -g -w -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv -ftabstop=4 $(INC) $(DEFS)
CFLAGS := -O1 -w $(INC) -DFT2_BUILD_LIBRARY -DNDEBUG
OBJS := $(OUT)/cr_qimagescale.o $(CRSRC:%=$(OUT)/cr_%.o) $(ZSRC:%=$(OUT)/z_%.o) $(foreach f,$(FTSRC),$(OUT)/ft_$(subst /,_,$(f)).o)

all: $(OUT)/cre-oracle.exe

$(OUT)/cre-oracle.exe: $(OUT)/oracle.o $(OBJS)
	$(CXX) -static -o $@ $^ -lgdi32 -lws2_32

$(OUT)/oracle.o: $(HERE)cre-oracle.cpp $(HERE)crsetup_oracle.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/cr_qimagescale.o: $(CRE)/crengine/qimagescale/qimagescale.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/cr_lvmemman.o: CXXFLAGS += -DCRE_ORACLE_NO_POSIX_SIGNALS

$(OUT)/cr_%.o: $(CRE)/crengine/src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/z_%.o: $(DEPS)/zlib-1.3.2/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

define FTRULE
$(OUT)/ft_$(subst /,_,$(1)).o: $(DEPS)/freetype-VER-2-14-3/src/$(1).c
	$$(CC) $$(CFLAGS) -c -o $$@ $$<
endef
$(foreach f,$(FTSRC),$(eval $(call FTRULE,$(f))))
