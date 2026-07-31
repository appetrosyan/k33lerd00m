/*
	Impactful monster death burst - emissive embers + rising smoke + a warm glow,
	sprayed along the killing-blow direction (the .fx axis is oriented from `dir`).
	Author: Aleksandr Petrosyan
*/
fx fx/rt_deathburst
{
	{
		delay		0
		duration	0.6
		restart		0
		light		"lights/spot01", 2.4, 1.2, 0.4, 220
		offset		0, 0, 16
		fadeOut		0.5
	}
	{
		delay		0
		duration	2.5
		restart		0
		particle	"rt_deathsmoke.prt"
	}
}
