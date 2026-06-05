CC=gcc
CFLAGS=`pkg-config --cflags libxfce4panel-2.0 gtk+-3.0 libnotify json-glib-1.0` `curl-config --cflags`
LDFLAGS=`pkg-config --libs libxfce4panel-2.0 gtk+-3.0 libnotify json-glib-1.0` `curl-config --libs`

all: 285AApanel.so

285AApanel.so: 285AApanel.o
	$(CC) -shared -o $@ $^ $(LDFLAGS)

285AApanel.o: 285AApanel.c
	$(CC) $(CFLAGS) -fPIC -c $<

clean:
	rm -f *.o *.so

.PHONY: all clean