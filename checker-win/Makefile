CC = cl
CFLAGS = /nologo /W3 /c

build: libscheduler.dll

libscheduler.dll: so_scheduler.obj glist.obj
	link /nologo /dll /out:$*.dll /implib:$*.lib $**

so_scheduler.obj: so_scheduler.c
	$(CC) $(CFLAGS) /Fo$@ $**

glist.obj: glist.c
	$(CC) $(CFLAGS) /Fo$@ $**

clean:
	del libscheduler.dll libscheduler.exp libscheduler.lib *.obj
