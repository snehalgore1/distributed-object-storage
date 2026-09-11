# vhs script for the failover demo GIF.
# Regenerate with:  vhs docs/failover-demo.tape   (from the repo root, after building)
Output docs/failover-demo.gif

Set FontSize 17
Set Width 1000
Set Height 800
Set Theme "Dracula"
Set Padding 18

Hide
Type "clear"
Enter
Show

Type "./build/cluster_demo"
Sleep 500ms
Enter
Sleep 12s
