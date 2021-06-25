build: libscheduler.so

libscheduler.so: glist.o so_scheduler.o
	gcc glist.o so_scheduler.o -o libscheduler.so -shared -Wall

so_scheduler.o: so_scheduler.c
	gcc so_scheduler.c -c -o so_scheduler.o -fPIC -Wall

glist.o: glist.c
	gcc glist.c -c -o glist.o -fPIC -Wall

.PHONY: clean
clean:
	rm -f *.o libscheduler.so
