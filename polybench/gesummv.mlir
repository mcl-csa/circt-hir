#bram_r = {"rd_latency"= 1}
#bram_w = {"wr_latency"= 1}
#reg_r  = {"rd_latency" = 0}
#reg_w  = {"wr_latency"= 1}

hir.func @gesummv at %t(
%alpha:i32, 
%beta:i32, 
%tmp:!hir.memref<8xi32> ports [#bram_w]> , 
%A:!hir.memref<8x8xi32>ports [#bram_r]>,
%B:!hir.memref<8x8xi32>ports [#bram_r]>,
%X:!hir.memref<8xi32>ports [#bram_r]>,
%Y:!hir.memref<8xi32>ports [#bram_w]>
){


  %0 = hw.constant 0:i4
  %1 = hw.constant 1:i4
  %4 = hw.constant 4:i4
  %5 = hw.constant 5:i4
  %6 = hw.constant 6:i4
  %8 = hw.constant 8:i4
  %9 = hw.constant 9:i4


  hir.for %i :i4 = %0  to %8 step %1  iter_time(%ti = %t  +  %1 ){
    %tmpreg_r,%tmpreg_w = hir.alloca("reg") :!hir.memref<1xi32,[0], #reg_r>,
                    !hir.memref<1xi32,[0], #reg_w>
    %yreg_r,%yreg_w = hir.alloca("reg") :!hir.memref<1xi32,[0], #reg_r>,
                    !hir.memref<1xi32,[0], #reg_w>

    hir.store %0 to %tmpreg_w[%0] at %ti 
      :(!hir.const, !hir.memref<1xi32, [0], #reg_w>[!hir.const])
    hir.store %0 to %yreg_w[%0] at %ti 
      :(!hir.const, !hir.memref<1xi32, [0], #reg_w>[!hir.const])
    
    %tf=hir.for %j :i32 = %0 :!hir.const to %8 :!hir.const step %1 :!hir.const 
      iter_time(%tj = %ti  +  %1 ){
        

        %a_i_j = hir.load %A[%i,%j] at %tj
        : !hir.memref<8x8xi32, #bram_r>[i32,i32] -> i32
        %b_i_j = hir.load %B[%i,%j] at %tj
        : !hir.memref<8x8xi32, #bram_r>[i32,i32] -> i32
        %x_j = hir.load %X[%j] at %tj
        : !hir.memref<8xi32, #bram_r>[i32] -> i32

        %t1 = hir.call @i32Multiplier(%a_i_j,%x_j) at %tj+%1 
          : !hir.func<(i32,i32)->(i32 delay 4)>
        %tmp_in = hir.load %tmpreg_r[%0] at %tj + %5
          : !hir.memref<1xi32,[0], #reg_r>[!hir.const] -> i32
        %tmp_next = hir.call @i32Adder(%t1,%tmp_in) at %tj+%5
          : !hir.func<(i32,i32)->(i32)>
        hir.store %tmp_next to %tmpreg_w[%0] at %tj+%5 
          : (i32, !hir.memref<1xi32, [0], #reg_w>[!hir.const])

        %t2 = hir.call @i32Multiplier(%b_i_j,%x_j) at %tj+%1
          : !hir.func<(i32,i32)->(i32 delay 4)>
        %y = hir.load %yreg_r[%0] at %tj + %5
          :!hir.memref<1xi32,[0], #reg_r>[!hir.const] -> i32
        %y_next = hir.call @i32Adder(%t1,%y) at %tj+%5
          : !hir.func<(i32,i32)->(i32)>
        hir.store %y_next to %yreg_w[%0] at %tj+%5 
          : (i32, !hir.memref<1xi32, [0], #reg_w>[!hir.const])
        hir.yield at %tj + %1
    }
    %tmp_in = hir.load %tmpreg_r[%0] at %tf + %5
      :!hir.memref<1xi32,[0], #reg_r>[!hir.const] -> i32
    hir.store %tmp_in to %tmp[%i] at %tf + %5 
      :(i32, !hir.memref<8xi32,  #bram_w>[i32])
    %y = hir.load %yreg_r[%0] at %tf + %5
      :!hir.memref<1xi32,[0], #reg_r>[!hir.const] -> i32
    %alpha_tmp = hir.call @i32Multiplier(%alpha,%tmp_in) at %tf+%5
      : !hir.func<(i32,i32)->(i32 delay 4)>
    %beta_y = hir.call @i32Multiplier(%beta,%y) at %tf+%5
      : !hir.func<(i32,i32)->(i32 delay 4)>
    %y_next = hir.call @i32Adder(%alpha_tmp,%beta_y) at %tf+%9
      : !hir.func<(i32,i32)->(i32)>

    %i9 = hir.delay %i by %9 at %ti : i32 -> i32
    hir.store %y_next to %Y[%i9] at %tf + %9
      :(i32, !hir.memref<8xi32,  #bram_w>[i32])

    hir.yield at %tf + %5
  }

  hir.return
}
