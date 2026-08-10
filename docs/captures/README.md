# Bus captures

Drop raw sniffs here as `.log` files, one frame per line, in the format
`main.cpp`'s snoop handler prints:

```
   14231  08 STATUS 04 00 00 00 00 00
   14295  08 MSG    50 4F 4F 4C ...  |POOL TEMP 78|
```

Captures are the only ground truth this project has for the parts marked
unverified in AGENTS.md. A capture from a real panel is worth more than any
amount of reasoning about what the protocol probably does.

Name them by panel model and revision, e.g. `rs8-combo-revT2.log`.
