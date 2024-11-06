CXX			= gcc 
CXXFLAGS	= -g -Wall -fsanitize=address -pthread
OBJS		= server.o main.o
PROG		= server

all:		$(PROG)

$(PROG):	$(OBJS)
		$(CXX) -o $(PROG) $(OBJS)

clean:;		$(RM) $(PROG) core *.o
