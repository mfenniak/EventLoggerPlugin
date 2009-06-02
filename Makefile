
CPP=g++
BASE_CFLAGS=-DVPROF_LEVEL=1 -DSWDS -D_LINUX -DLINUX -DNDEBUG -fpermissive -Dstricmp=strcasecmp -D_stricmp=strcasecmp -D_strnicmp=strncasecmp -Dstrnicmp=strncasecmp -D_snprintf=snprintf -D_vsnprintf=vsnprintf -D_alloca=alloca -Dstrcmpi=strcasecmp -march=pentium4
CPPFLAGS=$(BASE_CFLAGS) -m32 -Ipublic -Ipublic/tier0 -Ipublic/tier1 -I/usr/include/postgresql

server_i486.so: EventLoggerPlugin.o public/tier0/memoverride.o
	$(CPP) -shared -m32 -o server_i486.so EventLoggerPlugin.o public/tier0/memoverride.o lib/linux/*.a ~/tf2/orangebox/bin/tier0_i486.so ~/tf2/orangebox/bin/vstdlib_i486.so ~/postgresql-8.3.7/src/interfaces/libpq/libpq.a -lcrypt

EventLoggerPlugin.o: EventLoggerPlugin.cpp
	$(CPP) -c -o EventLoggerPlugin.o $(CPPFLAGS) EventLoggerPlugin.cpp

public/tier0/memoverride.o: public/tier0/memoverride.cpp
	$(CPP) -c -o public/tier0/memoverride.o $(CPPFLAGS) public/tier0/memoverride.cpp

clean:
	rm -rf public/tier0/*.o
	rm -rf *.o
	rm -rf *.so

install:
	cp server_i486.so ~/tf2/orangebox/tf/addons/coolmod3/bin/

