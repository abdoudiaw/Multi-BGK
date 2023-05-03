# Directories
DIR=$(PWD)/
EXECDIR=$(DIR)exec/
OBJDIR=$(DIR)obj/
SRCDIR=$(DIR)src/

# clang compiler
CC=gcc
#CFLAGS = -O2 -Wall -I/opt/homebrew/opt/gsl/include  -I/opt/homebrew/opt/open-mpi/include/
CFLAGS = -O2 -Wall -I/opt/homebrew/include -I/opt/homebrew/opt/gsl/include -I/opt/homebrew/opt/libomp/include

#LIBFLAGS = -lm -lgsl -lgslcblas -lomp -lmpi

LIBFLAGS = -lm -L/opt/homebrew/opt/gsl/lib -lgsl -lgslcblas -L/opt/homebrew/opt/libomp/lib -lomp -L/opt/homebrew/opt/open-mpi/lib -lmpi

#LIBFLAGS = -lm -lgsl -lgslcblas -L/opt/homebrew/opt/libomp/lib -lomp -lmpi


# Command definition
RM=rm -f

# sources for main
sources_main = $(SRCDIR)main.c

objects_main = BGK.o momentRoutines.o transportroutines.o poissonNonlinPeriodic.o gauss_legendre.o input.o io.o zBar.o initialize_sol.o mesh.o implicit.o TNB.o

pref_main_objects = $(addprefix $(OBJDIR), $(objects_main))

sources_postproc = $(SRCDIR)TNB_postprocess.c $(SRCDIR)TNB.c $(SRCDIR)io.c $(SRCDIR)input.c

# linking step
MultiBGK: $(pref_main_objects) $(sources_main)
	@echo "Building Multispecies BGK code"
	$(CC) $(CFLAGS) -o $(EXECDIR)MultiBGK_ $(sources_main) $(pref_main_objects) $(LIBFLAGS)

postProc: $(sources_postproc)
	@echo "Building TNB postprocessor"
	$(CC) $(CFLAGS) -o $(EXECDIR)postProc_ $(sources_postproc) $(LIBFLAGS)


all: MultiBGK postProc

$(OBJDIR)%.o : $(SRCDIR)%.c
	@echo "Compiling  $< ... " ; \
	if [ -f  $@ ] ; then \
		rm $@ ;\
	fi ; \
	$(CC)  -c $(CFLAGS)  $< -o $@ 2>&1 ;

$(OBJDIR)TNB.o : $(SRCDIR)TNB.c
	@echo "Compiling  $< ... " ; \
	if [ -f  $@ ] ; then \
		rm $@ ;\
	fi ; \
	$(CC)  -c $(CFLAGS)  $< -o $@ 2>&1 ;

clean:
	$(RM) $(OBJDIR)*.o
	$(RM) $(EXECDIR)*_
