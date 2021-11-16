const static char html_blob[] PROGMEM = R"====(
<html>
<head>
<meta name="viewport" content="width=device-width, user-scalable=no">
<style type="text/css">

html {
    background-color: #e4ded4;
    background-image: -webkit-linear-gradient(hsla(0,0%,0%,.1), hsla(0,0%,100%,.1));
    box-shadow: inset 0 0 100px hsla(0,0%,0%,.1);
    height: 100%;
}


body {
    font-family: "Ubuntu", sans-serif;
}

span.buttonlable{
    width:90px;
    height:100%;
    display: inline-block;
    vertical-align: top;
    padding-top: 5px;
    text-align: right;
}

select, option {
    text-align: center;
}

.prettydrop {
    width: 100px;
    height: 50px;
    padding: 5px;
    background-color: #eee;
    background-image: -webkit-linear-gradient(hsla(0,0%,100%,.1), hsla(0,0%,0%,.1));
    border-radius: 25px;
    outline: none;
}

.prettyinput {
    width: 150px;
    height: 50px;
    padding: 5px;
    background-color: #eee;
    background-image: -webkit-linear-gradient(hsla(0,0%,100%,.1), hsla(0,0%,0%,.1));
    border-radius: 25px;
    outline: none;
    box-shadow: inset 0 1px 1px 1px hsla(0,0%,100%,1),
                inset 0 -1px 1px 1px hsla(0,0%,0%,.25),
                0 1px 3px 1px hsla(0,0%,0%,.5),
                0 0 2px hsla(0,0%,0%,.25);

}

.prettyinput:active {
  background-color: #0f0;
}


input[type="checkbox"] {
    background-image: -webkit-linear-gradient(hsla(0,0%,0%,.1), hsla(0,0%,100%,.1)),
                      -webkit-linear-gradient(left, #f00 50%, #0CDB4F 50%);
    background-size: 100% 100%, 200% 100%;
    background-position: 0 0, 15px 0;
    border-radius: 25px;
    box-shadow: inset 0 1px 4px hsla(0,0%,0%,.5),
                inset 0 0 10px hsla(0,0%,0%,.5),
                0 0 0 1px hsla(0,0%,0%,.1),
                0 -1px 2px 2px hsla(0,0%,0%,.25),
                0 2px 2px 2px hsla(0,0%,100%,.75);
    cursor: pointer;
    height: 25px;
    /*left: 50%;*/
    /*margin: -12px -37px;*/
    padding-right: 25px;
    position: relative;
    /*top: 50%;*/
    width: 75px;
    -webkit-appearance: none;
    -webkit-transition: .15s;
    outline: none;
}
input[type="checkbox"]:after {
    background-color: #eee;
    background-image: -webkit-linear-gradient(hsla(0,0%,100%,.1), hsla(0,0%,0%,.1));
    border-radius: 25px;
    box-shadow: inset 0 1px 1px 1px hsla(0,0%,100%,1),
                inset 0 -1px 1px 1px hsla(0,0%,0%,.25),
                0 1px 3px 1px hsla(0,0%,0%,.5),
                0 0 2px hsla(0,0%,0%,.25);
    content: '';
    display: block;
    left: 0;
    position: relative;
    top: -5px;
    width: 50px;
    height: 35px;
}
input[type="checkbox"]:checked {
    background-position: 0 0, 35px 0;
    padding-left: 25px;
    padding-right: 0;
}

#container {
  width:500px; margin-left: auto; margin-right:auto; padding-top: 60px
}

#mainpowerdiv, #switchesdiv {
  height:40px; width:100%;
}

#switchesdiv div {
  display: inline-block;
}

@media (max-width: 800px) {

  #container { width: 100%; }

  #switchesdiv { overflow: auto; height:250px; }

  #mainpowerdiv, #switchesdiv div {
    display: block;
    font-size: 1.3em;
    height: 80px;
    text-align: center;
    line-height: 80px;
  }
  #mainpowerdiv span.buttonlable, #switchesdiv span.buttonlable {
    width: 45%;
  }

}

</style>

<script src="http://ajax.googleapis.com/ajax/libs/jquery/1.10.2/jquery.min.js"></script>

</head>

<body onload=updateStatus()>

<div id="container">
  <div id="mainpowerdiv">
    <span class="buttonlable">Power:</span>
    <span style="width: 75px"><input type="checkbox" 
          id="mainpowerbut" value="Power" onclick="psu(this)"></span>
  </div>
  <div id="switchesdiv">
    <div>
      <span class="buttonlable">Heating:</span>
      <span style="width: 75px"><input type="checkbox" id="chbut" 
          onclick="heating(this)"></span>
    </div>
    <div>
      <span class="buttonlable">Hot water:</span>
      <span style="width: 75px"><input type="checkbox" id="hwbut" 
          onclick="hotwater(this)"></span>
    </div>
  </div>
 
  <div id="extractrl" style="width:100%; height:auto">
    <span style="width: 100%; height:30px; display:block; float:left; cursor: pointer; text-decoration: underline; font-size:60%;" onclick="showhide(timedropdown)">More options &#x25bc;</span>
    <div id="timedropdown" style="display:none;">
      <span id="hwofftimesp" style="display:block;">HW Off:<span id="hwofftime">Unknown</span></span>
      <span id="chofftimesp" style="display:block;">CH Off:<span id="chofftime">Unknown</span></span>
      <select class="prettydrop" id="timesel">
        <option value="30">30 mins</option>
        <option value="90">90 mins</option>
        <option value="120">2 hours</option>
        <option value="180">3 hours</option>
      </select>
      <button type="button" class="prettyinput" 
          onclick=hotwater("on",document.getElementById('timesel').value)>HOT
           WATER</button>
      <button type="button" class="prettyinput" 
          onclick=heating("on",document.getElementById('timesel').value)>CENTRAL
           HEATING</button>
    </div>
  </div>

  <hr />
  <div id="hwtank" style="width:100%; height: 350px">
    <svg viewbox="0 0 640 900" preserveAspectRatio="xMidYMid meet">
    <defs id="defs4">
    <linearGradient id="linearGradient3758">
      <stop
         style="stop-color:rgba(0,0,255,1);stop-opacity:1;"
         offset="0"
         id="stop3760" />
      <stop
         id="stop3766"
         offset="0.5"
         style="stop-color:rgba(255,0,255,1);stop-opacity:1;" />
      <stop
         style="stop-color:rgba(255,0,0,1);stop-opacity:1;"
         offset="1"
         id="stop3762" />
    </linearGradient>
    <linearGradient
       inkscape:collect="always"
       xlink:href="#linearGradient3758"
       id="linearGradient3764"
       x1="431.00446"
       y1="915.44421"
       x2="431.00446"
       y2="214.9399"
       gradientUnits="userSpaceOnUse" />
  </defs>
  <g transform="translate(-231.71875,-207.375)">
    <path
       style="fill:url(#linearGradient3764);fill-opacity:1;stroke:#000000;stroke-width:10;stroke-linejoin:bevel;stroke-miterlimit:4;stroke-opacity:1;stroke-dasharray:none"
       d="m 438.5625,212.375 c -111.59143,0 -201.40625,89.81482 -201.40625,201.40625 l 0,146.71875 -0.4375,0 0,355.15625 403.71875,0 0,-355.15625 
-0.4375,0 0,-146.71875 C 640,302.18982 550.15393,212.375 438.5625,212.375 z" id="rect2987" inkscape:connector-curvature="0" />
    </g>
    </svg>
  </div>
</div>

<script>
var ch_status = '';
var hw_status = '';
var psu_status = '';
var debug = false;
var testing = false;

function logger(message){
    if (debug == true) {
        console.log(message);
    };
};

function updateStatus() {
  var statuses = {};
  var endpoints = [
    {url_part: "hw", button_id: "hwbut", spanid: "hwofftime"},
    {url_part: "ch", button_id: "chbut", spanid: "chofftime"},
    {url_part: "psu", button_id: "mainpowerbut"}
  ];
  endpoints.forEach(function(thing) {
    $.getJSON("get/" + thing.url_part, function(result) {
      document.getElementById(thing.button_id).checked = result.state;
      if('undefined' != typeof result.offtime){
        var date = new Date(result.offtime*1000);
        document.getElementById(thing.spanid).innerHTML=date;
      };
    });
  });
}

function psu(object){
    state=object.checked;
    logger(object.id+" is "+state);
    if (true == state && true != testing) {
        $.ajax({url:'set/psu/on', type:'POST', data:''})
    } else if (false == state && true != testing){ 
        $.ajax({url:'set/psu/off', type:'POST', data:''})
    }

};

function hotwater(object, duration){
  if (typeof object == "object") {
    state = object.checked;
    logger(object.id+" is "+state)
  } else if (typeof object == "string") {
    state = object;
    logger("HW state is "+object);
  };
  if (duration != null) {logger(duration)};
  if ((true == state || state == "on") && null == duration && true != testing) {
    //$.ajax({url:'make_full_tank.py', type:'GET',data:''});
    $.ajax({url:'set/hw/on', type:'POST', data:''})
  } else if ((true == state || state == "on") && null != duration && true != testing) {
    $.ajax({url:'set/hw/on/'+duration, type:'POST', data:''})
  } else if ((false == state || state == "off") && true != testing){ 
    $.ajax({url:'set/hw/off', type:'POST', data:''})
  }
};

function heating(object, duration){
  if (typeof object == "object") {
        state = object.checked;
        logger(object.id+" is "+state)
  } else if (typeof object == "string") {
        state = object;
        logger("CH state is "+object);
  };
  if (duration != null) {logger(duration)};
  if ((true == state || state == "on") && null == duration && true != testing) {
    $.ajax({url:'set/ch/on', type:'POST', data:''})
  } else if ((true == state || state == "on") && null != duration && true != testing) {
    $.ajax({url:'set/ch/on/'+duration, type:'POST', data:''})
  } else if ((false == state || state=="off") && true != testing){
    $.ajax({url:'set/ch/off', type:'POST', data:''})
  }
};

function showhide(object){
    $(object).toggle("clip");
};


function loadhwdata(){
/*
  var hwdata = [];
  $.getJSON("get/hwc", function(data){
    data.rows.forEach(function(r){
      var sensiblerow = {};
      for (var i=0; i<data.cols.length; i++){
        sensiblerow[data.cols[i].label] = r.c[i].v;
      };
      hwdata.push(sensiblerow);
    });
    logger(hwdata);
    setStops(hwdata);
  });
*/

    $.getJSON( "get/hwc", function(data) {
    logger(data);
    setStops(data);
    });
};

function setStops(hwdata){
  // Convert them to colours
  var topcolour = colourFromTemp(hwdata.top);
  var middlecolour = colourFromTemp(hwdata.mid);
  var bottomcolour = colourFromTemp(hwdata.btm);

logger(topcolour);
logger(middlecolour);
logger(bottomcolour);


  // Set the colour stop colours in the SVG to the colours we worked out
  var colourstops = document.querySelectorAll("svg stop");
  logger(colourstops);
  colourstops[0].style.stopColor = bottomcolour;
  colourstops[1].style.stopColor = middlecolour;
  colourstops[2].style.stopColor = topcolour;
 
//colourstops[0].style.stopColor = "rgb(0,255,0)";
//colourstops[1].style.stopColor = "rgb(0,255,0)";
//colourstops[2].style.stopColor = "rgb(0,255,0)";

};

function colourFromTemp(temp) {
    var t = temp;
    if (t < 20) t = 20;
    if (t > 55) t = 55;
    t = t - 20; // so it's now a value between 0 and 35
    redness = Math.round(t * 255 / 35);
    blueness = Math.round(255 - (t * 255 / 35));
    //return "rgba(" + redness + ",0," + blueness + ",0)";
    return "rgb(" + redness + ",0," + blueness+")";
};


if (testing) {setInterval(updateStatus, 10000);} else {setInterval(updateStatus, 3000)};
loadhwdata();
var d = setInterval(loadhwdata, 600 * 1000);
</script>
</body>
</html>
)====";
