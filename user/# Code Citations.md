# Code Citations

## License: unknown
https://github.com/givenone/os-pa4/tree/379531b03fb730da283be47b1e39681b56f8284b/kernel/vm.c

```
char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte &
```

