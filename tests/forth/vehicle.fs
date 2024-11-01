\ weForth Vehicle demo
s" forth/shapes.fs" included
1000   constant ID                     \ body id
: bike
  0.4 0.5 0.8 px 3!                    \ bike body px[width, height, length]
  ID ds !                              \ bike id
  0 0 0 ds .pos 3! 0 0 0 1 ds .rot 4!  \ pos[3], rot[4]
  $00ff00 3 px DSZ ds                  \ create bike body
  s" bike %x %p %p" JS
  150 10000 1000 px 3!                 \ set engine params
  ID 3 px s" engine %x %p" JS
  2 8000 2000 px 3!                    \ set transmission params
  ID 3 px s" gearbox %x %p" JS ;
: wheel ( n -- ) ds .id !              \ keep wheel index
  ID 3 px DSZ ds                       \ create front wheel
  s" wheel %x %p %p" JS ;
: front_wheel ( -- )
  0 -0.2 0.65    ds .pos 3!            \ pos[x,y,z]
  1.5 0.3 0.5    ds .lv  3!            \ suspension[freq, min, max]
  30 rad dup 500 ds .av  3!            \ steering, caster, break strength
  0.3 0.3 0.08   px 3!      0 wheel ;  \ front wheel dim[r1, r2, width]
: back_wheel ( -- )
  0 -0.2 -0.95   ds .pos 3!            \ pos[x,y,z]
  2.0 0.3 0.5    ds .lv  3!            \ suspension[freq, min, max]
  0 dup 250      ds .av  3! 1 wheel ;  \ back wheel, steering[ang, caster, break]
: car
  1.2 0.3 2.0 px 3!                    \ car body px[width, height, length]
  ID ds !                              \ body id
  0 10 0 ds .pos 3! 0 0 0 1 ds .rot 4! \ pos[3], rot[4]
  $00ff00 3 px DSZ ds                  \ create car body
  s" fwd %x %p %p" JS
  800 10000 1000 px 3!                 \ set engine params
  ID 3 px s" engine %x %p" JS
  2 8000 2000 px 3!                    \ set transmission params
  ID 3 px s" gearbox %x %p" JS ;
: car_wheels ( -- )
  0.8 0.1 1.2    ds .pos 3!            \ relative pos[x,y,z]
  1.5 0.3 0.5    ds .lv  3!            \ suspension[freq, min, max]
  30 rad dup 500 ds .av  3!            \ angle[steering, caster], break strength
  0.3 0.3 0.1    px 3!      0 wheel    \ FL wheel dim[r1, r2, width]
  -0.8 0.1 1.2   ds .pos 3! 1 wheel    \ FR wheel, pos[x,y,z]
  0.8 0.1 -1.2   ds .pos 3!            \ pos[x,y,z]
  0 dup 500      ds .av  3! 2 wheel    \ RL wheel, steering, caster, break strength
  -0.8 0.1 -1.2  ds .pos 3! 3 wheel ;  \ RR wheel, pos[x,y,z]
: start s" start" JS ID 1+ to ID ;
: go_bike
  bike front_wheel back_wheel start ;
: go_car
  car car_wheels start ;
: bikes for go_bike next ;
: cars for go_car next ;
.( vehicle.fs loaded )
