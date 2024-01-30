# weForth - Web eForth with WASM

WebAssembly enpowered eForth on web browsers. Is it faster? Is it more portable? Yes, and Yes.

Well, on my aged laptop, the impression is pretty exciting! It's about 2x faster than pure Javascript implementation on a browser and at 1/4 speed of C/C++ natively compiled code on CPU. On the portability end, though not exactly plug-and-play but some simple alteration turned my C code web-eabled. Of course, WASM has not integrated with the front-end well enough, updating DOM is a different feat.

Regardless, it brought me warm smiles seeing eForth run in a browser. Better yet, it's straight from C/C++ source code. Other popular scripting languages such as Python, Ruby are trending toward WASM/WASI implementation as well. However, depending solely on JIT without built-in compiler as Forth does, the interpreter-in-an-interpreter design will likely cap the top-end performance (i.e. stuck at 5%~10% of native, so far).

With WASM, the interoperability between different languages become a thing of the near future. If Forth can compile word directly into WASM opcodes, engage WASI to access OS and peripherals, hookup the graphic front-end (i.g. SDL or WebGL), weForth can become a worthy scripting alternative for Web.

### Features
* Javascript access to ss, dict, and VM memory (via WebAssembly.Memory)
* Forth in Web Worker threads (multi-VMs possible)
* IDE-style interactive front-end (cloud possible, i.g. JupyterLab)

### To Compile and Run (make sure python3 and Emscripten are installed)
* em++ -o tests/ceforth.html src/ceforth.cpp --shell-file template/ceforth.html -sEXPORTED_FUNCTIONS=_main,_forth,_vm_base,_vm_ss,_vm_ss_idx,_vm_dict_idx,_vm_dict,_vm_mem,_top -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
  > Note: -O2 works OK, -O3 emscripten spits wrong code
* Server-side
  > python3 tests/serv.py
* Client-side Browser
  > http://localhost:8000/tests/ceforth.html

### To Compile to Web Worker (run almost at the same speed as main thread)
* cp template/weforth.html template/weforth.css template/file_io.js template/weforth_helper.js template/weforth_worker.js tests
* em++ -o tests/weforth.js src/ceforth.cpp -sEXPORTED_FUNCTIONS=_main,_forth,_vm_base,_vm_ss,_vm_ss_idx,_vm_dict_idx,_vm_dict,_vm_mem,_top -sEXPORTED_RUNTIME_METHODS=ccall,cwrap -O2
* Server-side
  > python3 tests/serv.py
* Client-side Browser
  > http://localhost:8000/tests/weforth.html

### To Debug (dump all functions, check with wasm-objdump in WABT kit)
* em++ -o tests/ceforth.html src/ceforth.cpp --shell-file template/ceforth.html -sEXPORT_ALL=1 -sLINKABLE=1 -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
* wasm-objdump -x tests/ceforth.wasm > ceforth.wasm.txt

### Benchmark (on my aged IBM X230)
> Simple 1K*10K tests
>> : xx 9999 FOR 34 DROP NEXT ;<br/>
>> : yy 999 FOR xx NEXT ;<br/>
>> : zz MS NEGATE yy MS + ;<br/>
>> zz

* CPU = Intel i5-3470 @ 3.2GHz
* FFa = FireFox v120, FFa1 = FireFox v120+1-worker
* FFb = FireFox v122, FFb1 = FireFox v122+1-worker
* EMa = Emscripten v3.1.51
* EMb = Emscripten v3.1.53

|implementation|source code|optimization|platform|run time(ms)|code size(KB)|
|---|---|---|---|---|---|
|ceforth v8    |C         |-O0|CPU |266 |111|
|              |          |-O2|CPU |106 |83 |
|eForth.js v6  |JavaScript|   |FFa |756 |20 |
|uEforth v7.0.2|asm.js / C|?  |FFa |814 |?  |
|uEforth v7.0.7|asm.js / C|?  |FFb |1364|?  |
|weForth v1.2  |EMa / C   |-O0|FFa1|943 |254|
|              |          |-O2|FFa1|410 |165|
|weForth v1.4  |EMb / C   |-O0|FFb |515 |259|
|              |          |-O2|FFb |161 |168|
|weForth v1.4  |EMb / C   |-O0|FFb1|515 |259|
|              |          |-O2|FFb1|163 |168|

* Note1: uEforth v7 uses switch(op), instead of 'computed goto' (asm.js/WASM has no goto)
* Note2: weForth v1 uses token indirected threaded
* Note3: weForth+switch(op), is 2x slower than just function pointers.
* Note4: weForth v1.2 without yield in nest() speeds up 3x.
* Note5: WASM -O3 => err functions (wa.*) not found
* Note6: Chrome is about 10% slower than FireFox
       
### TODO
* review wasmtime (CLI), perf+hotspot (profiling)
* Forth CPU visualizer (with SDL2)
* GraFORTH spec.
  * File system (FS/IndexedDB)
  * Editor
  * 2D graphic (SDL)
  * Character graphic (HTML5)
  * 3D graphic (WebGL)
  * Music (SDL)
* add network system (wget/WebSocket)
* inter-VM communication
* use WASM stack as ss
* macro-assembler
