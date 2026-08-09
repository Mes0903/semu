# Keep local file targets aligned with the shared source-build artifact
# contract instead of spelling raw artifact filenames here. Query by class name
# so registry order changes cannot silently swap Make's semantic variables.
prebuilt_artifact_output = $(strip $(shell scripts/prebuilt/artifact-classes.sh output $(1)))
KERNEL_DATA := $(call prebuilt_artifact_output,image)
INITRD_DATA := $(call prebuilt_artifact_output,rootfs)
TEST_TOOLS_DATA := $(call prebuilt_artifact_output,test-tools)

$(if $(KERNEL_DATA),,$(error Failed to resolve prebuilt artifact output for image))
$(if $(INITRD_DATA),,$(error Failed to resolve prebuilt artifact output for rootfs))
$(if $(TEST_TOOLS_DATA),,$(error Failed to resolve prebuilt artifact output for test-tools))

PREBUILT_ARTIFACTS := $(KERNEL_DATA) $(INITRD_DATA) $(TEST_TOOLS_DATA)

PREBUILT_URL ?= https://github.com/sysprog21/semu/releases/download/prebuilt

# Local Make defaults to consuming the rolling release for missing guest
# artifacts. Source builds stay explicit through `make build-artifacts`; CI owns
# recipe-key/cache decisions separately under .ci/prebuilt.
define download_prebuilt_artifact
$(1):
	$$(Q)printf '  GET\t$$@\n'
	$$(Q)PREBUILT_URL="$$(PREBUILT_URL)" scripts/prebuilt/download-release-artifact.sh "$$@"
endef

$(foreach artifact,$(PREBUILT_ARTIFACTS),$(eval $(call download_prebuilt_artifact,$(artifact))))
