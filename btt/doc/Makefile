DOCTMP	= btt.log btt.aux btt.dvi btt.toc

all: btt.pdf

btt.tex:
	@touch btt.tex

btt.pdf: btt.tex
	@latex btt.tex
	@latex btt.tex
	@dvipdfm -p a4 btt
	@rm -rf $(DOCTMP)

clean:
	-rm -f btt.pdf $(DOCTMP)
