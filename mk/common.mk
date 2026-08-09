UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PRINTF = printf
else
    PRINTF = env printf
endif

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
    REDIR =
else
    Q := @
    VECHO = @$(PRINTF)
    REDIR = >/dev/null
endif

# Get specified feature
POSITIVE_WORDS = 1 true yes
define has
$(if $(filter $(firstword $(ENABLE_$(strip $1))), $(POSITIVE_WORDS)),1,0)
endef

# Set specified feature
define set-feature
$(eval CFLAGS += -D SEMU_FEATURE_$(strip $1)=$(call has, $1))
endef

# Test suite
PASS_COLOR = \e[32;01m
NO_COLOR = \e[0m

notice = $(PRINTF) "$(PASS_COLOR)$(strip $1)$(NO_COLOR)\n"
