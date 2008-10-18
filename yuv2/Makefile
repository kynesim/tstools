#
# Set JAVA_HOME to the location of your Java installation.
# Set SWTDIR to the location which contains your swt.jar.
#
# make will then build a yuv2.jar . 
# 
# bin/ contains a script wrapper you can use to invoke your
#  yuv2.jar . Edit bin/yuv2 to give the location of your yuv2.jar.
#

SRC_DIR=src/com/aminocom/yuv2
SRC_FILES=AboutBox.java BufferedImage.java FormatDialog.java \
	ImageCanvas.java LogWindow.java MainFrame.java Prefs.java \
	Utils.java YUV2.java

LOCATED_SRC=$(SRC_FILES:%.java=$(SRC_DIR)/%.java)


all:
	if [ ! -d obj ]; then mkdir obj; fi
	$$JAVA_HOME/bin/javac -cp $$SWTDIR/swt.jar -d obj -sourcepath bin $(LOCATED_SRC)
	(cd obj; $$JAVA_HOME/bin/jar -xf $$SWTDIR/swt.jar)
	(cd obj; $$JAVA_HOME/bin/jar -cmf ../src/Manifest-Extras.txt ../yuv2.jar *)

clean:
	rm -rf obj
	rm -f yuv2.jar


# End file.
