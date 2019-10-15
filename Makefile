CPPFLAGS=--std=c++11 -O3 -DNDEBUG=1
CXX=g++
RM=rm -f

all: blaze

blaze: src/blaze.o
	$(CXX) -o blaze src/blaze.o -lcurl -lpthread

blaze.o: src/blaze.cpp
	$(CXX) $(CPPFLAGS) -c src/blaze.cpp -o src/blaze.o

.PHONY: clean
clean:
	$(RM) src/blaze.o

.PHONY: distclean
distclean: clean
	$(RM) blaze

.PHONY: install
install: blaze
	mkdir -p $(DESTDIR)/usr/local/bin
	install -m 4755 -o root blaze $(DESTDIR)/usr/local/bin

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)/usr/local/bin/blaze
