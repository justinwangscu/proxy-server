CXX			= gcc
CXXFLAGS	= -g -Wall
OBJS		= proxy.o
PROG		= ps

all:		$(PROG)

$(PROG):	$(OBJS)
		$(CXX) -o $(PROG) $(OBJS)

clean:;		$(RM) $(PROG) core *.o
