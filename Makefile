COMPONENTS = gh_fe gh_db gh_dist gh_sh gh_ws gh_web

# Directories that need to be copied to the installation path.
SRC_DIRS = common \
		   db-handler \
		   dist-handler \
		   frontend-proxy \
		   session-handler \
		   websocket-handler \
		   web

# Directories where we need to run 'npm install'.
# 'npm install' will also be run at the top level.
NPM_DIRS = common \
		   db-handler \
		   dist-handler \
		   examples/js \
		   session-handler \
		   test \
		   websocket-handler \
		   web

.PHONY: required cpp npm examples test clean install uninstall

required:
	$(MAKE) npm
	$(MAKE) cpp

all:
	$(MAKE) required
	$(MAKE) examples

cpp:
	@echo Building session-handler.
	$(MAKE) -C session-handler all

npm:
	@echo Getting NPM dependencies.
	npm install
	$(foreach d, $(NPM_DIRS), cd $(d); npm install; cd -;)

examples:
	@echo Building C++ examples.
	$(MAKE) -C examples/cpp all

test:
	nodeunit test/unit.js

clean:
	$(MAKE) -C session-handler clean
	$(MAKE) -C examples/cpp clean



install:
	@echo Installing Greyhound...
#
# Copy module launchers.
	chmod -R 755 scripts/init.d/
	$(foreach comp, $(COMPONENTS), cp scripts/init.d/$(comp) /etc/init.d/;)
#
# Set up Greyhound component source directory.
	mkdir -p /var/greyhound/
#
# Set up Greyhound component logging directory.
	mkdir -p /var/log/greyhound/
#
# Make source directories for each component.
	$(foreach srcDir, $(SRC_DIRS), mkdir -p /var/greyhound/$(srcDir);)
#
# Copy component sources.
	$(foreach srcDir, $(SRC_DIRS), cp -R $(srcDir)/* /var/greyhound/$(srcDir);)
#
# Copy top-level dependencies.
	cp Makefile /var/greyhound/
	cp config.js /var/greyhound/
	cp forever.js /var/greyhound/
	mkdir -p /var/greyhound/node_modules/
	cp -R node_modules/* /var/greyhound/node_modules/
#
# Copy launcher utility.
	chmod 755 greyhound
	cp greyhound /usr/bin/



uninstall:
	@echo Removing all traces of Greyhound...
#
# Remove module launchers.
	$(foreach comp, $(COMPONENTS), rm -f /etc/init.d/$(comp);)
#
# Remove launcher utility.
	rm -f /usr/bin/greyhound
#
# Remove sources.
	rm -rf /var/greyhound/
#
# Remove log files.
	rm -rf /var/log/greyhound/

