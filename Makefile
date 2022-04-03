CC = gcc
TARGET = shellfyre

default: $(TARGET).c
	$(CC) -w -o shellfyre shellfyre.c 

        
clean:
	 $(RM) $(TARGET)

